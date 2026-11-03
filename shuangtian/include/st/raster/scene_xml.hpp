#pragma once

/// @file scene_xml.hpp
/// @brief **三维场景的 XML 形态**：与 `scene3d_script.hpp` 的 JSON 同一份契约的第二种前端。
///
/// ## 为什么需要它
///
/// 霜天已有「用 XML 描述界面」（`docs/xml-ui.md`）与「用 JSON 描述三维场景」
/// 两条通路。本模块补第三角：**用 XML 描述三维场景**——让「描述界面」与
/// 「描述模型」用同一种笔法，与 xml-ui 同构。
///
/// XML 相对 JSON 的表达优势恰好落在三维场景上：
///
/// | JSON 的别扭之处 | XML 的自然形态 |
/// |---|---|
/// | `objects` 是扁平数组，`children` 嵌套深了括号地狱 | **标签嵌套 = 层级**，一眼看出父子 |
/// | 数组必须整体给（改一个元素要重写整段） | 子元素逐个增删，互不影响 |
/// | `"shape": "sphere"` 形状藏在值里 | **标签名 = 图元**（`<Sphere>`），扫一眼即知 |
///
/// ## 形态（与 xml-ui 同构的三条规则）
///
/// 1. **标签 = 图元**：`<Sphere>` `<Box>` `<Helix>` …（22 个内置图元，见
///    `geometry.md` §2；大小写宽松）。非法标签按「未知图元」报错（列出全部可用名）。
/// 2. **嵌套 = 层级**：子元素映射 `children`（与扁平 `parent` 写法互斥，同 JSON）。
/// 3. **属性名与 JSON 字段逐字同名**：`light_from` `ground_y` `size` `translate`——
///    模型在两种格式间迁移零成本。三元组/颜色写逗号串：`translate="0, 0.4, 0"`。
///
/// ```xml
/// <Scene>
///   <Camera eye="0, 0.6, 3.4" target="0, 0, 0" fov="45"/>
///   <Lighting light_from="0.4, 0.8, 0.6" ambient="0.2"/>
///   <Shadow strength="0.45" ground_y="-0.55"/>
///   <Sphere name="head" size="0.9" color="#c8a07a" translate="0, 0.4, 0">
///     <Box name="eye" size="0.08, 0.08, 0.04" translate="0.2, 0.1, 0.85"/>
///   </Sphere>
///   <Animation name="wave" duration="2">
///     <Track node="head" kind="rotation">
///       <Key t="0" value="0, 0, 0" easing="ease-in-out"/>
///       <Key t="1" value="0, 0, 0.1"/>
///     </Track>
///   </Animation>
/// </Scene>
/// ```
///
/// ## 架构：第二前端，不是第二契约
///
/// 字段名、类型、范围、常见错误、默认值——全部复用 `scene_schema.hpp` 的
/// **唯一真源**字段表。本模块只做三件事：
///
/// 1. **XML 层检查**：未知属性（带**行号**——`xml::Node.line` 已有）与未知结构
///    标签，报错给编辑距离候选，绝不静默；
/// 2. **形态转换**：XML 树 → `Json`（结构标签/图元标签 → 对象/数组，属性 → 键）；
/// 3. **复用校验**：转出的 Json 交给 `parse_scene_spec`——类型检查、范围、
///    `shape` 合法性、动画节点引用等全部免费复用。
///
/// 这样字段表永远只有一份：JSON 加字段，XML 自动可用；XML 想加私有语法，
/// 必须先过字段表（否则两边漂移，正是 `scene_schema.hpp` 开头警告的那种事）。
///
/// ## 有意不做的事
///
/// - **不提供属性的 `keys` 写法**：动画关键帧只走 `<Key>` 子元素，不搞
///   `keys="0:0,0,0; 1:0,0,0.1"` 这类迷你语言——两套写法意味着两套解析与两套报错。
/// - **不嵌 UI**：`<Scene>` 是纯三维场景；「界面里放一块三维」用 `SceneView`
///   组件（那是 UI XML 的事），两者通过数据（`SceneSpec`）衔接，不互相包含。
/// - **不改动渲染层**：`SceneSpec → render_scene` 管线原样复用。

#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/xml/xml.hpp"

namespace st::raster {

/// 解析一段 XML 场景文本。
///
/// `source` 是完整的 XML 文档（根元素应为 `<Scene>`；给单个图元标签也接受，
/// 视为「只有 objects 的场景」——方便粘一段模型就预览）。
///
/// 报错带行号与节点路径（如 `第 7 行 <Sphere raduis>：未知属性；候选：radius`）。
[[nodiscard]] auto parse_scene_xml(std::string_view source) -> Result<SceneSpec>;

/// 解析已建好的 XML 树（`xml::parse` 的产物）。
///
/// 单独暴露是因为 xml-ui 的热重载已经持有 XML 树——同一棵树既要建界面
/// 又要建场景时，避免二次解析。
[[nodiscard]] auto parse_scene_xml_node(const xml::Node& root) -> Result<SceneSpec>;

/// 把场景序列化成 XML 文本（**规范形**，缩进两空格）。
///
/// 与 `serialize_scene`（JSON）同一口径：只写非默认值、必填项显式
/// （`objects` 恒写、每个对象的 `shape` 恒写）、顺序固定。输出必须能被
/// `parse_scene_xml` 原样读回且语义不变（往返测试保证）。
[[nodiscard]] auto serialize_scene_xml(const SceneSpec& spec) -> std::string;

/// 判断一段文本「像 XML 场景」（供 `st_api --check` 等入口自动分派）。
///
/// 判据：去 BOM/空白后以 `<` 开头（JSON 不可能），且根标签是 `Scene` 或
/// 任一已知图元/结构标签。返回 false 不代表「是合法 JSON」，只代表「不该走 XML 路」。
[[nodiscard]] auto looks_like_scene_xml(std::string_view source) -> bool;

}  // namespace st::raster
