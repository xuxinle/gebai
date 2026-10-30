#pragma once

/// @file api_manifest.hpp
/// @brief **机读能力清单**：让模型查表，而不是读几万字文档或凭训练数据猜。
///
/// ## 为什么需要一份"机读"清单
///
/// 本仓有 40 多份 `docs/*.md`，全是散文。模型要写三维代码时只能：
/// 读几万字（成本高、易漏）或凭训练数据猜（会写成 three.js 风格）。
/// 两者都会导致"第一次就写错"。
///
/// 这份清单是**结构化**的：每个能力有名字、类型、取值范围、示例、常见错误。
/// 模型可以只取自己需要的那一段，成本是几十行而不是几万字。
///
/// ## 为什么从 `scene_schema` 生成而不是手写
///
/// 手写两份（代码一份、清单一份）必然漂移，而"清单说支持、代码其实不读"
/// 是最难查的一类问题。所以清单**只有一份真源**（字段表），其余全部从它推。
///
/// ## 输出格式
///
/// JSON（`json_dump` 可直接落盘或经协议传递）。顶层结构：
///
/// ```json
/// {
///   "version": "1",
///   "generated_for": "shuangtian/raster/scene3d_script",
///   "primitives": { "names": [...], "aliases": {...} },
///   "tracks": ["translate", "rotate", "scale", "visible"],
///   "easings": ["linear", "constant", "ease-in", "ease-out", "ease-in-out"],
///   "scopes": {
///     "objects[]": {
///       "fields": [
///         { "name": "radius", "type": "number", "required": false, "range": "> 0",
///           "summary": "...", "example": "0.5", "pitfalls": ["..."] }
///       ]
///     }
///   },
///   "quickstart": { ... }     // 一段最小可运行脚本
/// }
/// ```

#include <string>

#include "st/core/json_fwd.hpp"

namespace st::raster {

/// 生成完整的能力清单（JSON 对象）。
[[nodiscard]] auto build_api_manifest() -> Json;

/// 生成清单的文本形式（`pretty` 为真时缩进）。
[[nodiscard]] auto api_manifest_text(bool pretty = true) -> std::string;

/// 一段**最小可运行**的场景脚本（供模型抄起步）。
///
/// 为什么单独暴露：模型最需要的是"能跑的最小样板"，而不是完整文档。
/// 这段脚本本身在测试里会被解析一遍，保证它一直是合法的——
/// 否则示例会随 API 演进悄悄失效，而失效的示例比没有示例更糟。
[[nodiscard]] auto minimal_example_script() -> std::string;

}  // namespace st::raster
