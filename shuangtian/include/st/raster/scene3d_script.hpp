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
#include "st/ext/json.hpp"
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

  /// \xe6\x98\xaf\xe5\x90\xa6\xe4\xbd\x9c\xe4\xb8\xba**\xe5\x9c\xb0\xe9\x9d\xa2**\xef\xbc\x88\xe6\x8e\xa5\xe6\x94\xb6\xe9\x98\xb4\xe5\xbd\xb1\xe3\x80\x81\xe4\xb8\x8d\xe9\x81\xae\xe6\x8c\xa1\xe9\x98\xb4\xe5\xbd\xb1\xef\xbc\x89\xe3\x80\x82
  ///
  /// JSON \xe9\x87\x8c\xe6\xb2\xa1\xe6\xb3\x95\xe8\xa1\xa8\xe8\xbe\xbe\xe3\x80\x8c\xe5\x85\x88\xe7\x94\xbb\xe8\xb0\x81\xe3\x80\x8d\xe8\xbf\x99\xe7\xa7\x8d\xe9\xa1\xba\xe5\xba\x8f\xe4\xbe\x9d\xe8\xb5\x96\xef\xbc\x8c\r
  /// \xe6\x89\x80\xe4\xbb\xa5\xe7\x94\xa8\xe4\xb8\x80\xe4\xb8\xaa\xe5\xad\x97\xe6\xae\xb5\xe6\x98\xbe\xe5\xbc\x8f\xe5\x8c\xba\xe5\x88\x86\xef\xbc\x8c\xe7\x94\xb1\xe6\xb8\xb2\xe6\x9f\x93\xe5\x99\xa8\xe8\xb4\x9f\xe8\xb4\xa3\xe6\x8e\x92\xe9\xa1\xba\xe5\xba\x8f\xe3\x80\x82
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

/// 生成一个对象的网格（`shape` + 形状参数）。
///
/// 暴露出来是为了让调用方能单独检查"我的参数生成了什么网格"，
/// 而不是只能渲一整帧再看。
[[nodiscard]] auto build_mesh(const SceneObjectSpec& object) -> Result<Mesh>;

}  // namespace st::raster
