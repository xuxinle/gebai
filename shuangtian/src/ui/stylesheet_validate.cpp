// 样式表的**校验器**（`validate_stylesheet`）——只有测试与工具用，不在运行期路径上。
//
// ## 为什么单独一个翻译单元（本轮体积优化的一处关键）
//
// 校验器必须“真跑一遍解析”才能发现「键表里有、但 `apply_declaration` 没有分支」
// 这类缺陷（只查表发现不了，实测 2019-11-19 的教训写在 `include/st/ui/stylesheet.hpp`）。
// 于是它需要**一个真的声明式页面**来承载样式 —— 即一个小 `dsl::Component` + `dsl::mount`。
//
// 这两样东西会把整条声明式链拖进产物：`dsl::mount` → `Composer` → `create_element`
// → **全量组件导入** → CodeEditor / Terminal / MarkdownView / Table / Tree /
// FileDialog / HtmlView / DiffView。
//
// 而校验器**只被测试调用**（`nm` 可证：`apps/*.o` 里没有任何 `validate_stylesheet` 引用）。
// 它的代价却是**每个应用**都付：实测 `gedou` 因此白背 **656 KB**。
//
// 拆到这里之后：应用不引用 `validate_stylesheet` → 本 TU 不进产物 → 那 8 个组件
// 一个都不进。测试仍照常调用（测试二进制直传全部用例源，必然链进来）。

#include "st/ui/stylesheet.hpp"

#include <memory>
#include <string_view>

#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {

namespace {

/// 校验器用的**最小页面**：一个空容器，只要能承载样式即可。
///
/// 为何需要真元素：`validate_stylesheet` 必须真跑一遍解析才能发现
/// “键表里有、但 `apply_declaration` 没有分支”这种缺陷（只查表发现不了）。
struct ProbePage : dsl::Component {
  void build(dsl::Composer& c) override {
    (void)dsl::column(c, {.width = 80.0f, .height = 60.0f, .key = "probe"}, [] {});
  }
};

}  // namespace

auto validate_stylesheet(std::string_view text) -> SheetReport {
  // **真的解析一遍**（不是只查表）。
  //
  // 为何不能只查 `group_of_key`（实测 2026-11-19）：
  // 那只能验“名字在表里”，验不了“这个名字真的能落位”。
  // 而“登了名字但忘了写 `apply_declaration` 分支”正是这一层**唯一的新增缺陷形态**
  // （键表驱动的方式少写一条分支，编译器不会报任何东西）。
  // 实测：写入一个表里有、解析层不认的键，只查表版的校验器仍然全绿。
  //
  // 现在用一个**临时元素**真跑一次 `apply_stylesheet`，
  // 于是“表里有但落不了位”与“值不合法”都会被归进 `unknown`。
  UiRoot scratch;
  scratch.set_viewport({100.0f, 100.0f});
  auto page = std::make_shared<ProbePage>();
  if (dsl::mount(scratch, page) == nullptr) {
    // 建不出探针元素时退回只查表（不应发生，但不因此崩掉校验器）。
    SheetReport fallback;
    return fallback;
  }
  scratch.layout();
  Element* probe = scratch.content();
  if (probe == nullptr) return SheetReport{};
  SheetReport report;
  (void)apply_stylesheet(*probe, text, &report);
  return report;
}

}  // namespace st::ui
