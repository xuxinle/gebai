#pragma once

/// @file scene_serialize.hpp
/// @brief **场景反向序列化**：`SceneSpec` → JSON（读→改→写闭环的前提）。
///
/// ## 为什么这是"大模型可用性"的关键一环
///
/// 没有它，模型无法：
/// - 读一份已有场景、改一处、写回（**增量修改**是最常见的工作方式）；
/// - 让引擎把"我理解成什么"吐出来对照"我写的是什么"（**理解差异**是最隐蔽的 bug）。
///
/// 有了它，"改动一个字段"从"重写整份脚本"变成"读→改→写"，而后者不容易碰坏
/// 其它部分——这一点对模型尤其重要，因为重写常常顺手改坏没打算碰的地方。
///
/// ## 往返一致性的口径（这条必须说清）
///
/// 往返**不是逐字符相等**，而是**语义等价**：
///
/// | 不保真的项 | 为什么 |
/// |---|---|
/// | 字段顺序 | JSON 对象无序；本仓用 `ordered_json` 保序，但语义与顺序无关 |
/// | 数值写法（`1.0` vs `1`） | 浮点输出的规范化 |
/// | 缺省字段 | 序列化只写**非默认值**（见下） |
///
/// **只写非默认值是刻意的**：写全量会让输出膨胀好几倍，而模型读的时候
/// 反而更难看出"这个场景到底改了什么"。默认值靠 `st_api` 清单可查。
///
/// 但有一个例外：**必须显式的项**。例如 `objects` 是必填（哪怕空数组），
/// 以及一个字段"存在"本身有语义时（`ground: true` 与"没写"不同）。
///
/// ## 一条必须遵守的约定
///
/// 序列化出的 JSON 必须能被 `parse_scene_spec` **原样读回**且语义不变。
/// 这一点由往返测试保证——它是这份代码唯一的正确性判据。
/// 漏写一个字段不会报错（读回时用默认值），但会被往返测试抓住。

#include <string>

#include "st/core/json_fwd.hpp"

namespace st::raster {

struct SceneSpec;
struct SceneObjectSpec;

/// 把场景序列化成 JSON 对象。
///
/// 顺序固定（`camera` → `lighting` → `shadow` → `cull` → `asset_basis` →
/// `skeleton` → `animation` → `objects`）：与 `st_api` 清单的 scope 顺序一致，
/// 便于人/模型对照。
[[nodiscard]] auto serialize_scene(const SceneSpec& spec) -> Json;

/// 序列化成文本（`pretty` 为真时缩进）。
[[nodiscard]] auto serialize_scene_text(const SceneSpec& spec, bool pretty = true) -> std::string;

/// 只序列化一个对象（含子节点）。用于"只想看这个物体被理解成了什么"。
[[nodiscard]] auto serialize_object(const SceneObjectSpec& object) -> Json;

}  // namespace st::raster
