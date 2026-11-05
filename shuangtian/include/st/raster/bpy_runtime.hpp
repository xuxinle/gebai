#pragma once

/// @file bpy_runtime.hpp
/// @brief **用 bpy（Blender 的 Python 三维建模 API）写三维场景**，与 three.js 那条路并列。
///
/// ## 为什么再加一个表达层
///
/// 本框架已有两条：`scene3d_script`（JSON）与 `ThreeRuntime`（three.js）。
/// 前者语料稀薄；后者在"一般三维场景"上很好，但在**精确建模**上不如 bpy：
///
/// | 要做的事 | three.js 好不好写 | bpy 好不好写 |
/// |---|---|---|
/// | 一个球浮在底座上 | 好 | 好 |
/// | 按模数生成 60 齿齿轮 | 要手算齿形顶点 | `bmesh` 逐顶点，与 Blender 语料同构 |
/// | 倒角、布尔、依附曲面 | 没有 | `bpy.ops` / 修改器 |
///
/// 模型的语料里 bpy 在**机械件/程序化几何/资产级建模**上远厚于 three.js。
/// 所以这一层不是替代，是补齐。
///
/// ## 关键设计：产出同一个 `ThreeScene`
///
/// ```text
/// three.js 脚本 ──→ ThreeRuntime::evaluate ──┐
///                                           ├──→ ThreeScene ──→ Scene3D / 裁云 / 两条渲染腿
/// bpy 脚本    ──→ BpySceneExtractor::extract ┘
/// ```
///
/// `ThreeScene` 是**来源无关**的对接点：下游零改动。这条设计让 bpy 这条
/// 路线的成本只落在"脚本 → 场景"这一段。
///
/// ## 实现方式：MicroPython 内嵌 + Python 实现的 bpy 兼容层
///
/// | 层 | 在哪 |
/// |---|---|
/// | 解释器 | MicroPython（`third_party/micropython/py/`，一字未改） |
/// | bpy 语义 | `src/python/*.py`（Python 实现，见 `docs/python.md`） |
/// | 抽取 | `src/python/extract.py`（Python）→ JSON → 本文件（C++ 搬数据） |
///
/// 抽取放在 Python 侧是刻意的：在 C++ 里逐层读 bpy 对象（每个属性一次
/// `mp_load_attr` + 类型检查）代码量极大且易错；Python 侧读自己的对象是
/// 自然表达，C++ 只做纯数据搬运。
///
/// ## 已知边界（详见 `docs/python.md`）
///
/// - **`@` 运算符不可用**：MicroPython 编译器不支持 PEP 465，用 `.matmul()` / `.dot()`
/// - 面三角化用**扇形**：凸多边形正确，凹多边形会出错
/// - 材质只取第 0 个槽（多材质面需要分组，暂不支持）
/// - 父子关系不抽（`matrix_world` 已经是绝对变换，父子只影响那个矩阵）
///
/// ## 用法
///
/// ```cpp
/// auto scene = st::raster::bpy_scene_from_script(R"PY(
///     import bpy
///     from mathutils import Vector
///     bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=1.0, depth=2.0)
///     bpy.context.active_object.material_slots.append(bpy.data.materials.new('steel'))
/// )PY");
/// if (scene.has_value()) { /* 交给 Scene3D 或裁云 */ }
/// ```

#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/raster/three_runtime.hpp"

namespace st::raster {

/// bpy 脚本 → `ThreeScene`。
///
/// 一次调用做三件事：跑脚本、抽取、转成 `ThreeScene`。
/// 返回的 `ThreeScene` 与 `ThreeRuntime::evaluate` 的产物**同构**，
/// 下游无法（也不需要）区分来源。
class BpySceneExtractor {
 public:
  /// bpy 运行时是否可用（MicroPython 能初始化、兼容层能装载）。
  [[nodiscard]] static auto available() -> bool;

  /// 不可用的**具体原因**（可用时返回空串）。
  ///
  /// 只给 bool 时"为什么"全被吞掉，排查只能靠猜——与 `ThreeRuntime`
  /// 同一立场。
  [[nodiscard]] static auto unavailable_reason() -> std::string;

  /// 跑脚本并抽成场景。
  ///
  /// `script` 是完整的 Python 源码（`import bpy` 开头的那种）。
  /// 脚本里把"要导出的东西"赋给 `result` 是**可选**的——
  /// 不写就导出场景里的全部 mesh 对象。
  [[nodiscard]] static auto extract(std::string_view script) -> Result<ThreeScene>;

  /// 便捷入口：脚本 → 场景（同 `extract`，语义化名字）。
  [[nodiscard]] static auto scene_from_script(std::string_view script)
      -> Result<ThreeScene> {
    return extract(script);
  }
};

/// 把抽取器返回的 JSON 转成 `ThreeScene`。
///
/// 单独暴露是给测试用的：可以构造一份 JSON 直接验搬运逻辑，
/// 不必每次都跑 Python。
[[nodiscard]] auto parse_extracted_json(std::string_view json) -> Result<ThreeScene>;

}  // namespace st::raster
