// 按名造组件的**功能面**保证（体积面见 `tools/check_component_pruning.py`）。
//
// ## 两层判据各管什么（别指望单测去量体积）
//
// “按名字造组件”要成立得同时是两件事，它们**只能用不同的手段测**：
//
// | 面 | 判据 | 在哪测 |
// |---|---|---|
// | 体积：不引用就不进产物 | `nm -C <exe>.debug` 查不到重型组件符号 | **产物级**：`tools/check_component_pruning.py` |
// | 功能：要引用就真能造出来 | `make_element(名)` 非空 | 单测（本文件） |
//
// 为什么体积**不能**写成单测：`st_tests` 直传所有用例源，必然链接了全部组件，
// 于是单测进程里“重型组件永远在”。实测过：把导入实现回退掉，本文件的断言
// 依然全绿——那正是 `CONVENTIONS.md` §7 说的**恒绿的测试**。
// 所以体积那一面交给产物级脚本（那里做过逆向验证：回退导入后它确实变红）。
//
// ## 本文件真正防的是什么
//
// 实测踩过的那次故障：把全量导入从 `dsl.cpp` 挪走后，`xiangqi` 的 XML 装树
// 失败（`认不出的标签 <Scroll>`）——**体积降了，应用不能用了**。
// “只测省体积”会奖励这种实现，所以功能面必须有断言盯着。

#include "st/test/test.hpp"

#include <cstdio>
#include <string>

#include "st/ui/components/all.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"

namespace {

using st::ui::dsl::make_element;
using st::ui::dsl::registered_element_types;

/// 重型组件：最容易“被顺手裁掉”的一批（它们只被少数应用用到）。
/// 判据用**注册名**（与 XML 标签 / 脚本 `ui.create` 写的是同一套名字）。
constexpr const char* kHeavyComponentNames[] = {
    "CodeEditor", "Terminal", "MarkdownView", "DiffView", "HtmlView", "Table", "Tree", "FileDialog",
};

}  // namespace

/// **注册表里的每个类型都必须能按名字造出来**。
///
/// `make_element` 是三条按名创建入口（XML 标签 / 脚本 `ui.create` / 协议 `create`）
/// 的共同终点。任何一项造不出来，那些界面就会**凭空少一块且不报错**。
ST_TEST(component_import_every_registered_type_is_constructible) {
  st::ui::import_all_components();
  const auto names = registered_element_types();
  ST_CHECK(names.size() >= 53U);  // 组件库规模下界（防清单被误删；53 = 49 + 日期时间四件）
  for (const std::string& name : names) {
    auto element = make_element(name);
    if (element == nullptr) {
      std::printf("[missing] 注册表有 '%s'，但 make_element 造不出来\n", name.c_str());
    }
    ST_CHECK(element != nullptr);
  }
}

/// 八个重型组件逐个点名：它们必须都能按名字造出来。
///
/// 与上一条互补：这条在“注册表刚好漏了某项”时**仍然有效**（点名而非枚举），
/// 而它们正是被误裁过一次的那批。
ST_TEST(component_import_heavy_components_are_constructible) {
  st::ui::import_all_components();
  for (const char* name : kHeavyComponentNames) {
    auto element = make_element(name);
    if (element == nullptr) std::printf("[missing heavy] %s\n", name);
    ST_CHECK(element != nullptr);
    if (element != nullptr) {
      // 造出来的元素要能报出自己的身份（工厂表与运行期类型一致）。
      ST_CHECK(!element->type().empty());
    }
  }
}
