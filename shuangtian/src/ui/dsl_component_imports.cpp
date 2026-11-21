// 「按名字造组件」的全量导入——**单独一个翻译单元**，它是整条按需链的闸门。
//
// ## 它解决什么问题
//
// “按名字造组件”（XML 标签 / `ui.create("Tree")` / 控制协议 `create`）与
// “C++ 里直 `new`”是两条完全不同的链接需求：
//
// - 直 `new`：那个组件的 `.o` 因被引用而进产物，静态登记自动完成 —— 天然按需；
// - 按名字：编译期根本不知道会造哪个，**必须有人对导入锚点产生真实引用**，
//   否则 `create_element("Tree")` 会得到「认不出的标签」。
//
// 后者一定意味着“整套组件都要在产物里”。而它是**可选能力**。
//
// ## 谁该引用它（这是本设计的全部要点）
//
// 由**真正按名创建的那几条路径**显式引用，而不是 `dsl` 核心：
//
// | 入口 | 是否可能需要按名创建 | 引用本文件 |
// |---|---|---|
// | `xml_ui.cpp`（`build_from_xml`） | 是（`<Terminal>` 这类标签） | ✓ |
// | `script_bridge.cpp`（`ui.create("Tree")`） | 是 | ✓ |
// | `control/server.cpp`（协议 `create`） | 是 | ✓ |
// | `dsl::make_element` 本身 | **否**：它只是被上面几处调用的工具 | ✗ |
// | 直 `new` 组件的应用 | 否 | ✗ |
//
// 反例（第一版做错的地方）：把它挂在 `dsl.cpp` 的 `make_element` 上——那是**强引用**，
// 于是任何链接了 `dsl.o` 的应用都会把它拉进来，闸门永远关不上。改成
// “静态初始化器自注册”也不行：没人引用时那个 `.o` 根本不进链接，注册器永不运行，
// XML 应用会拿到「认不出的标签」（实测 xiangqi 装树失败）。
//
// ## 判据（可复算）
//
// - 不用 XML / 脚本 / 协议的应用：`nm -C <产物>.debug | grep 'component_importer<st::ui::Table>::run'` 应为 0；
// - 用 XML 的应用：`<Scroll>` 之类标签必须能建成（起一次 `--headless` 看有没有“认不出的标签”）。

#include "st/ui/components/all.hpp"
#include "st/ui/dsl.hpp"

namespace st::ui::dsl {

void ensure_builtin_components_imported() {
  // 一次性的全量导入。惰性而非静态对象：跨 TU 静态初始化顺序不可控，
  // 而调用点（`build_from_xml` / 脚本桥 / 协议 create）总是在“真的要用”之前。
  static const bool imported = []() {
    import_all_components();
    return true;
  }();
  (void)imported;
}

}  // namespace st::ui::dsl
