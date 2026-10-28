/// `Tree` 大行数（1000 行）成本量尺 —— 回答 `DESIGN.md §11` 里那条 **`tree`（1000 节点）< 5 ms
/// / *未测*** 的悬空目标。
///
/// ## 为什么需要这个探针（而不是"读代码推断"）
///
/// `Tree` 是**自绘**组件：行不是子 `Element`，而是 `std::vector<Row>` 扁平数组。它因此
/// **不经过** `Element::paint` 的视口剔除（那只覆盖子元素树），代价全藏在
/// `measure`（逐行 `port.measure_width`）与 `paint_content`（逐行绘制）两个循环里。
/// 这类成本**读代码只能定性**（"是 O(n)"），要变成可比较的数字必须实测。
///
/// ## 量什么（三口尺子，口径与应用实际一致）
///
/// | 尺子 | 对应应用里的哪一步 | 期望 |
/// |---|---|---|
/// | `measure` | `sync_nodes` 之后的重排（刷新文件树/展开目录） | 应与行数无关（行宽只取决于最长标签） |
/// | `paint(400px 视口)` | 每帧绘制（视口只能容纳约 15 行） | 应与**可见行数**相关，与总行数无关 |
/// | `paint(全量)` | 同上，视口能容纳全部行时 | 应与行数线性 |
///
/// 三口尺子必须分开：合成一个"tree 耗时"会把「刷新贵」与「每帧贵」两类不同现象互相掩盖。
///
/// ## 用法
///
/// ```bash
/// tools/build_one_probe.sh ui_tree_rows_bench     # 仅验证用；不进 st.pkg
/// build/probe/ui_tree_rows_bench [行数]
/// ```
///
/// 需要系统字体（`FontStack::system_default()`）——无字体环境会打印 `SKIP` 并以 0 退出。

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

auto elapsed_ms(Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// 造 `count` 行"文件树"——标签长度贴近真实工程（不是全 `x`，否则量的是常量折叠）。
[[nodiscard]] auto make_rows(std::size_t count) -> std::vector<st::ui::TreeNode> {
  std::vector<st::ui::TreeNode> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const bool is_dir = (index % 7) == 0;
    const int depth = static_cast<int>(index % 4);
    std::string label = is_dir ? "components_layer_" : "translation_unit_";
    label += std::to_string(index);
    label += is_dir ? "" : ".cpp";
    rows.push_back(st::ui::TreeNode{.key = "node/" + std::to_string(index),
                                    .label = std::move(label),
                                    .expanded = is_dir,
                                    .is_dir = is_dir,
                                    .depth = depth});
  }
  return rows;
}

/// 取多次最小值（抖动与缓存都只会让它变大，最小值是最接近真实成本的估计）。
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
      .title = "`Tree` 大行数（1000 行）成本量尺 —— 回答 `DESIGN.md §11` 里那条 **`tree`（1000 节点）< 5 ms",
      .criterion = "",  // 判据由人补（脚本不猜合格线）
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("ui_tree_rows_bench", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  const std::size_t rows = argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 1000U;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts.has_value()) {
    st::log::out("SKIP: 无系统字体，量尺不成立（measure_width 恒 0 会量出假结果）\n");
    return 0;
  }
  st::text::TextRenderer renderer{*fonts};
  auto port = st::app::make_text_port(renderer);

  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::RenderContext context{theme, port.get(), 0.0};

  st::ui::Tree tree;
  const auto nodes = make_rows(rows);

  // —— 尺子①：measure（刷新后的重排）——
  tree.sync_nodes(nodes);
  tree.arrange(context, st::math::Rect{0.0f, 0.0f, 280.0f, 400.0f});  // 先定一次，避免首测含冷缓存
  const double measure_ms = best_ms(20, [&] {
    tree.measure(context, st::ui::Constraints{});
    tree.arrange(context, st::math::Rect{0.0f, 0.0f, 280.0f, 400.0f});
  });

  // —— 尺子②：paint（视口 400px 高 —— 只能容纳约 15 行）——
  st::raster::Canvas viewport_canvas = st::raster::Canvas::for_logical_size(280, 400, 1.0f);
  tree.measure(context, st::ui::Constraints{});
  tree.arrange(context, st::math::Rect{0.0f, 0.0f, 280.0f, 400.0f});
  const double paint_viewport_ms = best_ms(10, [&] {
    viewport_canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    tree.paint(context, viewport_canvas);
  });

  // —— 尺子③：paint（画布高到能容纳全部行 —— 上界情形）——
  const int full_height = static_cast<int>(static_cast<float>(rows) * 26.0f) + 40;
  st::raster::Canvas full_canvas = st::raster::Canvas::for_logical_size(280, full_height, 1.0f);
  tree.arrange(context, st::math::Rect{0.0f, 0.0f, 280.0f, static_cast<float>(full_height)});
  const double paint_full_ms = best_ms(10, [&] {
    full_canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    tree.paint(context, full_canvas);
  });

  // —— 尺子④：sync_nodes（刷新成本，含按 key 对齐）——
  const double sync_ms = best_ms(10, [&] { tree.sync_nodes(nodes); });

  // —— 尺子⑤：**冷**一次（先清空本地文本缓存观感，再量一轮）——
  //
  // 为何要分开：尺子②③都是 `best_ms`（多次取最小值）——**热**起来之后整形缓存命中，
  // 量到的是"缓存有效时"的成本。而真实编辑器的首个大目录是**冷**的：
  // 第一次 `measure` 要为每一行付一次整形。两个数不是同一件事，合并会互相掩盖。
  // （不做真正的缓存清空——那需要改文本层的 API；这里用"新构造一个 Tree +
  //   首轮不预热"近似冷局，并如实标注这是近似值。）
  st::ui::Tree cold_tree;
  cold_tree.set_row_height(tree.row_height());
  cold_tree.sync_nodes(nodes);
  const auto cold_start = Clock::now();
  cold_tree.measure(context, st::ui::Constraints{});
  const double cold_measure_ms = elapsed_ms(cold_start);

  st::log::out("rows={}  行高={:.0f}  视口=400px(约 {} 行)\n", rows, tree.row_height(),
            static_cast<std::size_t>(400.0f / tree.row_height()));
  st::log::out("  sync_nodes       {:8.3f} ms\n", sync_ms);
  st::log::out("  measure+arrange  {:8.3f} ms\n", measure_ms);
  st::log::out("  paint(400px)     {:8.3f} ms   <- 每帧真实成本（视口只显示十几行）\n",
            paint_viewport_ms);
  st::log::out("  paint(全量{:5}px) {:8.3f} ms   <- 上界：视口高到装下全部行\n", full_height,
            paint_full_ms);
  st::log::out("  measure(冷首轮)   {:8.3f} ms   <- 近似值：整形缓存未预热的首轮\n",
            cold_measure_ms);
  // 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）

  return st::probe::main_of(spec, opts, verdict);
}
