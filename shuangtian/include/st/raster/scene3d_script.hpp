#pragma once

/// @file scene3d_script.hpp
/// @brief 用一个 JSON 对象描述**整个三维场景**，再一次性渲染出来。
///
/// ## 为何需要这一层（面向 AI 原生使用）
///
/// 现在的 `Scene3D` 是一组**过程式调用**：`create` → `set_camera` →
/// `set_lighting` → `set_material` → `draw_mesh` × N → `end_frame`。
/// 顺序错了（比如在 `begin_frame` 前设材质）会静默失效，而调用方
/// （尤其是生成代码的模型）很难自己发现。
///
/// 本模块把「一个场景」变成**一份可校验的数据**：
///
/// ```json
/// {
///   "camera":  { "eye": [0, 0.6, 3.4], "target": [0, 0, 0], "fov": 45 },
///   "lighting": { "key_direction": [0.4, 0.8, 0.6], "ambient": 0.2,
///                 "diffuse": 0.6,
///                 "extra_lights": [{"direction": [-0.6,-0.2,0.5], "intensity": 0.5}] },
///   "shadow":  { "strength": 0.45, "ground_y": -0.55 },
///   "objects": [
///     { "shape": "sphere", "radius": 0.9, "color": "#c8a07a",
///       "metallic": 0.0, "roughness": 0.2, "specular": 1.0,
///       "translate": [0, 0.4, 0], "rotate": [0, 0, 0] }
///   ]
/// }
/// ```
///
/// 好处具体有三条：
/// 1. **顺序由代码保证**：调用方不可能把 `set_material` 排到 `begin_frame` 前面；
/// 2. **可校验**：字段名拼错、类型不对、颜色格式非法都会返回**带位置可查**的错误，
///    而不是静默用默认值；
/// 3. **可复现**：同一份 JSON 两次渲染逐像素相同（无随机、无时间依赖）。
///
/// ## 与裁云的关系
///
/// 裁云的 `model3d` 图层是**面向视频**的（有自己的 `Layer` 字段与时间轴）。
/// 本模块是**框架层**的通用入口：不依赖裁云，任何应用（包括测试与探针）
/// 都能用。两者不重复——裁云可以把这份 JSON 作为 `model3d` 的一个字段透传。
///
/// ## 有意不做的事
///
/// - **不支持任意网格文件**：`shape` 只接受内置生成器（sphere/box/plane/
///   cylinder/torus 的有无见 `Mesh` 的静态工厂）。外部 OBJ 需要文件系统与
///   路径解析，那是调用方的事——先 `Mesh::parse_obj`，再自己 `draw_mesh`。
/// - **不做动画**：这份描述是「一个静止场景」。时间轴属于上层（裁云的
///   `layout`），在这里引入时间会让确定性变复杂。

#include <string>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"

namespace st::raster {

/// 一个对象的描述（已从 JSON 解析成结构体，便于调用方检查）。
struct SceneObjectSpec {
  std::string shape{"sphere"};
  math::Color color{200, 200, 200, 255};
  math::Vec3 translate{0.0f, 0.0f, 0.0f};
  math::Vec3 rotate{0.0f, 0.0f, 0.0f};   ///< 欧拉角（弧度），顺序 X→Y→Z
  math::Vec3 scale{1.0f, 1.0f, 1.0f};

  // 形状参数（按 shape 取用）
  float radius{0.8f};
  float width{1.0f};
  float height{1.0f};
  float depth{1.0f};
  int segments{24};

  // 材质
  float metallic{0.0f};
  float roughness{1.0f};
  float specular{0.0f};
  float emission{0.0f};
  float opacity{1.0f};

  // ── 自发光色（2026-10-24，对齐 Blender 的 Emission Color 与 Strength 分离）──
  /// 自发光颜色（未设时 = 固有色，即旧行为）。
  math::Vec3 emissive_color{1.0f, 1.0f, 1.0f};
  bool emissive_color_set{false};
  /// true = 纯发光体（不受光照影响，如霓虹灯管 / 屏幕）。
  bool emission_unlit{false};

  /// 是否作为**地面**（接收阴影、不遮挡阴影）。
  ///
  /// JSON 里没法表达「先画谁」这种顺序依赖，所以用一个字段显式区分，
  /// 由渲染器负责排顺序。
  bool ground{false};
};

/// 完整场景描述。
struct SceneSpec {
  Camera camera{};
  Lighting lighting{};
  PlanarShadow shadow{};
  CullMode cull_mode{CullMode::Back};
  std::vector<SceneObjectSpec> objects{};
};

/// 把 JSON 解析成 `SceneSpec`。
///
/// ## 错误信息要能直接指路
///
/// 所有失败都带**字段路径**（如 `objects[1].roughness`）。理由很实际：
/// 这份 JSON 通常是模型生成的，它需要一个能直接定位的报错，而不是
/// 「解析失败」四个字。
///
/// ## 未知字段**不报错**
///
/// 多传的字段被忽略（不产生警告）。这是有意的：调用方常把同一个 JSON
/// 同时喂给多个消费者（裁云 / 探针 / 上层工具），各自认识一部分字段。
/// 但如果**必填字段**缺失或类型错误，会明确报错。
[[nodiscard]] auto parse_scene_spec(const Json& json) -> Result<SceneSpec>;

/// 从 JSON 文本解析。
[[nodiscard]] auto parse_scene_spec_text(std::string_view text) -> Result<SceneSpec>;

/// 按描述渲染一帧到画布。
///
/// 内部负责 `create` / `begin_frame` / 各 `set_*` / `draw_mesh` / `end_frame`
/// 的**正确顺序**——这正是本层的核心价值。
///
/// @param spec 场景描述
/// @param canvas 目标画布（尺寸决定场景分辨率）
/// @param background 清屏色（线性值）
[[nodiscard]] auto render_scene(const SceneSpec& spec, Canvas& canvas,
                                math::Color background) -> Result<void>;

/// 用**调用方提供的场景实例**渲染（不新建、不碰环境变量）。
///
/// ## 何时必须用它（实测踩到死锁）
///
/// `render_scene` 每次都 `Scene3D::create`，而 `create` 会**读写环境变量**
/// `ST_SCENE3D_BACKEND` 来选后端。若宿主（如裁云）自己也在改这个变量
/// （它的 `scene_slot` 为了强制软件腿会 `_putenv_s`），再嵌一层 `create`
/// 就会破坏宿主的保存/恢复逻辑——实测现象：**渲染到 33/180 帧卡死，
/// 控制通道完全超时**。
///
/// 宿主已有场景实例时用本函数；场景实例的尺寸要与 `canvas` 一致。
[[nodiscard]] auto render_scene_into(const SceneSpec& spec, Scene3D& scene, Canvas& canvas,
                                     math::Color background) -> Result<void>;

/// 生成一个对象的网格（`shape` + 形状参数）。
///
/// 暴露出来是为了让调用方能单独检查"我的参数生成了什么网格"，
/// 而不是只能渲一整帧再看。
[[nodiscard]] auto build_mesh(const SceneObjectSpec& object) -> Result<Mesh>;

}  // namespace st::raster
