#pragma once

/// 元素操作（读取 / 改属性 / 触发动作）——**C++ 应用、控制通道、脚本宿主共用的唯一实现**。
///
/// 为什么要独立成一层：这三条路径若各写一份，就会"同一个元素三个样"——实践里已经踩过两次
/// （① 脚本 `ui_get` 看不到属性面而协议 `get` 看得到；② `set` 的白名单与脚本 `ui_set` 不一致）。
/// 因此把操作语义钉在 `ui` 层，上层（control / script）只做参数解析与结果包装。
///
/// 坐标与数值一律走 `st::Json`；本层不依赖 `control`（避免 ui ← control 的反向依赖）。

#include <string>
#include <string_view>
#include <vector>

// `Json` 只出现在签名里 → 用**前向头**（不拉入 nlohmann 的 25,526 行）。
// 需要构造/访问 Json 的 .cpp 自行包含 `st/ext/json.hpp`。见 `st/ext/json_fwd.hpp`。
#include "st/ext/json_fwd.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {

/// 矩形 → JSON（`{x,y,width,height}`，**逻辑像素**）。
[[nodiscard]] auto rect_to_json(math::Rect rect) -> st::Json;

/// 元素基本信息（id/type/role/bounds/text/value）——不含属性面。
[[nodiscard]] auto element_to_json(Element& element) -> st::Json;

/// 元素快照 = 基本信息 + **属性面**（`props`：`property_names()` 逐个读 + enabled/visible）。
///
/// 控制通道的 `get` 与脚本的 `ui_get` 都返回它，保证"看到的完全一致"。
[[nodiscard]] auto element_snapshot(Element& element) -> st::Json;

/// 应用一批属性，返回**实际生效**的属性名数组。
///
/// 未识别的属性名被忽略（不报错）：调用方常批量下发，其中个别属性不被该组件支持是正常的。
[[nodiscard]] auto apply_properties(UiRoot& root, Element& element, const st::Json& properties)
    -> st::Json;

/// 触发元素动作（`click`/`focus`/`blur`/`submit`/…）。
///
/// `focus`/`blur` 必须经 `UiRoot` 设置：键盘事件按 root 的焦点元素派发，
/// 只改元素自身的 `focused` 标志会导致后续输入无处可送。
[[nodiscard]] auto invoke_element(UiRoot& root, Element& element, std::string_view action,
                                  std::string_view argument) -> bool;

/// 选择器查询（`limit` 为 0 表示不限制）。
[[nodiscard]] auto query_elements(UiRoot& root, std::string_view selector, std::size_t limit)
    -> Result<std::vector<Element*>>;

}  // namespace st::ui
