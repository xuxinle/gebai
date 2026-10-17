/// `List` 大项数量尺 —— 与 `ui_tree_rows_bench` 同一套口径，量 **List** 的那份。
///
/// ## 为什么 List 需要单独的量尺（它不是 Tree 的重复）
///
/// `Tree` 是**自绘**组件（行是 `std::vector<Row>` 数据，不是元素）——它的缺口在
/// `paint_content`/`measure` 里的手写循环。
///
/// `List` 正相反：每项是**真 `ListItem` 子元素**（`children_`）。它因此**不覆写**
/// `measure`/`arrange`/`paint`，全部走 `Element` 基类的递归：
///
/// | 阶段 | 基类行为 | 代价来源 |
/// |---|---|---|
/// | `measure` | 逐个子元素 `child->measure()` | N 次 `ListItem::measure` → N 次 `measure_width` |
/// | `arrange` | 逐个子元素 `child->arrange()` | N 次（便宜，但仍 N 次虚调用） |
/// | `paint` | 逐个子元素 `child->paint()` | N 次**剔除判定**（越界者提前 return，不真画） |
///
/// 也就是说：`paint` 有元素级剔除兜着（屏幕外的项**不画**），但**仍要付 N 次遍历与
/// 判定**；而 `measure` 完全没有剔除——**N 个项的文本全部量一遍**，还要撞文本层
/// 宽度缓存 4096 条的上限（见 `ui_tree_rows_bench` 里那个断崖）。
///
/// ## 量什么
///
/// | 尺子 | 对应真实场景 |
/// |---|---|
/// | `measure` | 刷新/重组后重排（声明式每帧都会走） |
/// | `arrange` | 同上 |
/// | `paint(400px)` | 每帧绘制（视口只容约 10 项） |
/// | `paint(全量)` | 上界：视口高到装下全部项 |
/// | `measure(冷首轮)` | 近似值：整形缓存未预热的首轮 |
///
/// ## 用法
///
/// ```bash
/// tools/build_ui_probe.sh ui_list_items_bench
/// build/probe/ui_list_items_bench [项数]
/// ```

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

auto elapsed_ms(Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// 造 `count` 条列表数据——文案长度贴近真实的文件/搜索结果列表。
[[nodiscard]] auto make_entries(std::size_t count) -> std::vector<st::ui::List::Entry> {
  std::vector<st::ui::List::Entry> entries;
  entries.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    st::ui::List::Entry entry;
    entry.key = "item/" + std::to_string(index);
    entry.label = "src/ui/components/file_" + std::to_string(index) + ".cpp";
    entry.subtitle = "modified " + std::to_string(index) + " seconds ago";
    entries.push_back(std::move(entry));
  }
  return entries;
}

template <class Fn>
[[nodiscard]] auto best_ms(int rounds, Fn&& fn) -> double {
  double best = 1e9;
  for (int round = 0; round < rounds; ++round) {
    const auto start = Clock::now();
    fn();
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "`List` 大项数量尺 —— 与 `ui_tree_rows_bench` 同一套口径，量 **List** 的那份。",
      .criterion = "",  // 判据由人补（脚本不猜合格线）
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("ui_list_items_bench", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  const std::size_t items = argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 1000U;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts.has_value()) {
    st::print("SKIP: 无系统字体，量尺不成立（measure_width 恒 0 会量出假结果）\n");
    return 0;
  }
  st::text::TextRenderer renderer{*fonts};
  auto port = st::app::make_text_port(renderer);

  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::RenderContext context{theme, port.get(), 0.0};

  st::ui::List list;
  const auto entries = make_entries(items);
  list.sync_items(entries);

  const float row_height = st::ui::ListItem::kHeight + 2.0F;  // 项高 + 默认间距
  const float content_height = static_cast<float>(items) * row_height + 8.0F;

  // 预热一次（让元素树/缓存进入稳态），再开始量。
  list.measure(context, st::ui::Constraints{});
  list.arrange(context, st::math::Rect{0.0F, 0.0F, 280.0F, content_height});

  const double measure_ms = best_ms(20, [&] {
    list.measure(context, st::ui::Constraints{});
    list.arrange(context, st::math::Rect{0.0F, 0.0F, 280.0F, content_height});
  });

  // 视口 400px（约 10 项可见）；List 自身高到内容全高（与真实 ScrollView 子元素一致）。
  st::raster::Canvas viewport_canvas = st::raster::Canvas::for_logical_size(280, 400, 1.0f);
  const double paint_viewport_ms = best_ms(10, [&] {
    viewport_canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    list.paint(context, viewport_canvas);
  });

  const int full_height = static_cast<int>(content_height) + 40;
  st::raster::Canvas full_canvas = st::raster::Canvas::for_logical_size(280, full_height, 1.0f);
  list.arrange(context, st::math::Rect{0.0F, 0.0F, 280.0F, static_cast<float>(full_height)});
  const double paint_full_ms = best_ms(10, [&] {
    full_canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    list.paint(context, full_canvas);
  });

  // 冷首轮：新 List 刚建完的第一次 measure（整形缓存未预热）。
  st::ui::List cold;
  cold.sync_items(entries);
  const auto cold_start = Clock::now();
  cold.measure(context, st::ui::Constraints{});
  const double cold_measure_ms = elapsed_ms(cold_start);

  st::print("items={}  视口=400px(约 {} 项)\n", items, static_cast<std::size_t>(400.0F / row_height));
  st::print("  measure+arrange  {:8.3f} ms   <- 刷新/重组后重排\n", measure_ms);
  st::print("  paint(400px)     {:8.3f} ms   <- 每帧真实成本（视口只显示十几项）\n",
            paint_viewport_ms);
  st::print("  paint(全量{:5}px) {:8.3f} ms   <- 上界：视口高到装下全部项\n", full_height,
            paint_full_ms);
  st::print("  measure(冷首轮)   {:8.3f} ms   <- 近似值：整形缓存未预热的首轮\n",
            cold_measure_ms);
  // 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）

  return st::probe::main_of(spec, opts, verdict);
}
