// `dsl::custom<T>` 在**按需链接**下必须自己声明“我要 T”。
//
// ## 这条测试防的是什么（本轮唯一的真回归）
//
// `custom<T>` 是编译期就知道类型的入口，但它曾经只做
// `create_element(element_type_name<T>(), ...)`——而 `element_type_name<T>()`
// 查的是**运行期 token 表**，那张表由组件的 `.cpp` 在静态初始化期填。
//
// 按需链接把那个 `.cpp` 裁掉之后（没人引用它），表里查不到 → 名字为空 →
// `create_element` 拿到 nullptr → **abort**。
// 实测：`caiyun` 启动即死，日志是 `custom<T> 未注册的组件类型: `（名字是空串）。
//
// ## 修法的两条分支（这里各钉一条）
//
// | 类型 | 判定 | 动作 |
// |---|---|---|
// | 内置组件 | `component_meta<T>` 有全特化（`ST_UI_COMPONENT`） | `import_one<T>()` |
// | 外部注册类型 | 无 `component_meta<T>` | 不导（它自己的 `.cpp` 本就会因注册被引用）|
//
// 第二条是必须钉的：**无条件**导入外部注册类型会链接期报未定义
// （实测 `caiyun::DirectorBridge`）。所以要有 `is_builtin_component<T>` 这个判据。
//
// > 说明：本文件用 `Panel` 作为“内置组件”样本来验证**判据本身**；
// > “按需链接下真的会丢”那一面只能在产物上测（`tools/check_component_pruning.py`），
// > 因为单测进程直连全部组件。

#include "st/test/test.hpp"

#include <memory>
#include <string>

#include "st/ui/component_registry.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/element.hpp"

namespace {

/// 一个**外部注册**类型的样本（没有 `component_meta` / `component_importer`）。
///
/// 结构上比真实外部组件（`caiyun::MonitorView`）更简，但判据完全同类：
/// 它跑的是 `register_element_type<T>` 那条路，而不是 `ST_UI_COMPONENT`。
class ExternalProbeElement : public st::ui::Element {
 public:
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ExternalProbe"; }
};

}  // namespace

/// 内置组件：`is_builtin_component` 必为真（否则会被跳过导入 → 运行期得空）。
ST_TEST(custom_builtin_component_is_detected) {
  ST_CHECK(st::ui::is_builtin_component<st::ui::Panel>::value);
  ST_CHECK(st::ui::is_builtin_component<::st::ui::Button>::value);
}

/// 外部注册类型：必须**不**被当成内置（否则会调到不存在的 `run()` → 链接失败）。
ST_TEST(custom_external_component_is_not_detected_as_builtin) {
  ST_CHECK(!st::ui::is_builtin_component<ExternalProbeElement>::value);
}
