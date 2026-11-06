#pragma once

/// 裁云 · **图层属性描述表**（`PropertyDesc` 在真实组件上的第一个采用）。
///
/// ## 这份表的意义
///
/// 它是“属性描述契约”从抽象变成可用的那块落点：检查器面板**不再手写控件**，
/// 而是遍历这份表生成。于是：
///
/// · 加一个可调参数 = 往表里加一行（+ `get/set_layer_property` 各一条分支），
///   而不是“再加一段 slider 代码”；
/// · 面板的取值范围**必然**与写入时的夹取一致（同一个描述）；
/// · 智能体能经控制通道自省到同样的元数据（`layer_schema_json`）。
///
/// ## 只描述“能被程序读写”的量（契约纪律 2）
///
/// 例如 `kind`（图层类型）不进表：改类型意味着整层形状语义都变
/// （`model3d` 与 `text` 没有共同的字段集），那不是“改一个参数”，
/// 而是“换一个图层”。面板上应该做成动作（按钮），不是控件。
///
/// ## 与 `schema.cpp` 的分工（两张表，不要混）
///
/// · `schema.cpp`：**JSON 脚本**的字段元数据（给写脚本的人/AI 看，键名与 JSON 一致）
/// · 本文件：**运行期图层对象**的可调参数（给面板用，也经控制通道暴露）
///
/// 两者必然高度重叠，但**不能合并**：脚本字段是“用户写什么”，
/// 图层属性是“对象能改什么”，前者有别名/百分比字符串等写法层信息，
/// 后者要的是类型与范围。合并会让两边都夹带对方的概念。

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "st/ui/property_schema.hpp"

namespace caiyun {

/// 名字与 `get_layer_property` / `set_layer_property` 逐字一致。
[[nodiscard]] auto layer_property_schema() -> std::span<const st::ui::PropertyDesc>;

/// 读一个可调属性（返回空 = 该属性不存在）。
///
/// `Layer` 需要完整定义——但本头文件不想把 `layer.hpp` 拖进每个 includeer，
/// 而 `Layer` 是个大结构（三维/文字/路径字段全在里面）。用 `void*` 会毁掉类型安全，
/// 所以这里**直接前置声明 + 传引用**，实现在 `.cpp` 里 include `layer.hpp`。
/// 调用方反正已经有 `Layer`。
struct Layer;

[[nodiscard]] auto get_layer_property(const Layer& layer, std::string_view name)
    -> std::optional<std::string>;

/// 写一个可调属性；返回是否生效。
///
/// **夹取范围来自 `layer_property_schema()`**，不在本函数里另写一套：
/// 否则“面板拖到端点”与“脚本写 999”会得到不同结果，而两者看起来都“成功”。
auto set_layer_property(Layer& layer, std::string_view name, std::string_view value) -> bool;

/// 把整张描述表导成 JSON（控制通道 `layer_schema_json` 用）。
[[nodiscard]] auto layer_schema_json() -> std::string;

}  // namespace caiyun
