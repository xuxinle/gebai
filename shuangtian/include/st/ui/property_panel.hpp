#pragma once

/// 霜天 · **由属性描述生成面板**（`PropertyDesc` → 控件）。
///
/// ## 它解决什么
///
/// 没有它时，加一个可调参数要改两处：组件的 `get/set_property`，
/// 以及某个面板 `build()` 里那一段 `slider(...)`。漏一处就是
/// 「能设但界面上没有」或「界面上有但设了不生效」——两者都只表现为
/// 「这个控件好像没什么用」，极难定位。
///
/// 有了它：**只写描述表**，面板自动出现对应控件。
///
/// ## 不做什么（边界）
///
/// · **不改进/不改值**：它只负责“把描述变成控件 + 把用户改动转成字符串回调”。
///   真正的写入仍走 `Element::set_property` / 应用自己的数据层——
///   单一真相源不变。
/// · **不生成布局策略**：控件纵向排列、`advanced` 折叠，但不做分组/标签页
///   （不同应用的信息架构差异太大，硬做会变成谁都不合用的框架）。
///
/// ## 三条纪律
///
/// 1. **控件类型由 `PropertyType` 决定**，不由调用方逐个指定——
///    否则“描述说 Number、面板给输入框”这类不一致又会回来。
/// 2. **数值控件必须用描述里的范围**（`min/max/step`），不另取一套。
/// 3. **只读属性生成禁用控件**（而不是不生成）：看见了但点不动，
///    比“看不见”更容易理解——用户不会以为功能缺失。

#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "st/ui/property_schema.hpp"

namespace st::ui {
class Element;
}

namespace st::ui::dsl {
class Composer;
}

namespace st::ui {

/// 生成面板时的回调集合。
///
/// 读值用 `read`（返回当前值的**字符串形式**，与 `get_property` 同口径）；
/// 改值用 `write`（收到新值字符串，由调用方解析并落到数据层）。
struct PropertyPanelHooks {
  /// 读当前值。返回 `nullopt` 表示“这个属性当前不可读”（控件显示占位）。
  std::function<std::optional<std::string>(std::string_view name)> read;
  /// 写入新值。返回是否生效（`false` 时面板不改显示，等下一次重读）。
  std::function<bool(std::string_view name, std::string_view value)> write;
  /// id 前缀：避免同屏两个面板的控件 id 撞车（控制通道按 id 定位）。
  std::string id_prefix{"prop"};
};

/// 把一组属性描述生成为控件的选项。
struct PropertyPanelOptions {
  /// 是否显示 `advanced` 属性（默认折叠 = 不显示）。
  bool show_advanced{false};
  /// 是否显示只读属性（默认显示但禁用——见文件头纪律 3）。
  bool show_readonly{true};
  /// 面板是否可编辑（`false` 时全部控件禁用；用于“编辑锁”这类整体门禁）。
  bool editable{true};
  /// 每行控件前的标签宽度（0 = 不显示标签，只显示控件）。
  float label_width{96.0f};
};

/// 生成控件（追加到 `composer` 当前容器）。
///
/// 返回**实际生成的控件数量**——调用方可用它判断“这个元素是不是没有可调参数”
/// （为 0 时该显示一句说明，而不是留一片空白）。
auto build_property_panel(dsl::Composer& composer, std::span<const PropertyDesc> descs,
                          const PropertyPanelHooks& hooks,
                          const PropertyPanelOptions& options = {}) -> std::size_t;

}  // namespace st::ui
