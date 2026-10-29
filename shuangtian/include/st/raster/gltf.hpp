#pragma once

/// @file gltf.hpp
/// @brief **glTF 2.0** 读写（`.gltf` + `.glb`）——通用的三维交换格式。
///
/// ## 为什么是 glTF
///
/// glTF 是 Khronos 的三维传输格式，被称作"三维界的 JPEG"：
///
/// | 消费者 | 支持情况 |
/// |---|---|
/// | Blender | 原生导入/导出（`File ▸ Import/Export ▸ glTF 2.0`） |
/// | Unity / Unreal / Godot / Cocos | 原生导入 |
/// | three.js / babylon.js / model-viewer / `<model-viewer>` | 原生 |
/// | Windows 3D 查看器 / macOS Preview / Office 3D | 原生 |
/// | Sketchfab / Vectary / Spline | 原生 |
///
/// 对"大模型生成三维"这件事，这一条比任何自研格式都重要：
/// **模型见过的 glTF 样本最多**，写出来的字段名与结构最可能一次就对。
/// 而 OBJ（本框架已有加载器）在纯几何上更简单，适合"只要形状"的场合——
/// 两者互补，都在本模块里覆盖。
///
/// ## 支持范围（诚实声明，不假装完整）
///
/// ✅ 已支持
/// - `.gltf`（JSON + 外部/内嵌 `.bin`）与 `.glb`（单文件二进制容器）
/// - `accessor` 的 `POSITION` / `NORMAL` / `TEXCOORD_0`，`indices`（无索引时按顺序三角形）
/// - 节点变换（`matrix` 或 `translation`/`rotation`/`scale`）与嵌套 `children`
/// - 材质：`pbrMetallicRoughness` 的 `baseColorFactor` / `metallicFactor` /
///   `roughnessFactor` / `emissiveFactor` / `alphaMode`（映射到 `Material`）
/// - 相机（`perspective` / `orthographic`）
/// - 多 mesh / 多 primitive → 合并进一个 `Mesh`（当前渲染器是"一网格一材质"）
/// - `data:` URI（base64）内嵌缓冲与图片
///
/// ❌ **不支持**（遇到时不静默出错，而是给出明确原因或跳过该项）
/// - 动画（`animations`）、蒙皮（`skins`）、变形目标（`morph targets`）
/// - `KHR_draco_mesh_compression`、`KHR_texture_basisu`（需要额外解码器）
/// - 纹理图像解码：图片字节会读出，但**不自动解码成像素**（PNG 解码在 `codec` 层，
///   由调用方决定用不用；这样 glTF 层不依赖解码器）
/// - 稀疏 accessor（`sparse`）、`TEXCOORD_1` 及以上、点/线图元（只做三角形）
///
/// ## 坐标与单位
///
/// glTF 规定 **+Y up、右手系、米**——与霜天三维完全一致，导入导出**不做轴变换**。
/// （OBJ 转换到 glTF 的常见坑就是 Y-up/Z-up 互换，这里从源头上不存在。）
///
/// ## 大模型友好
///
/// 所有错误信息都带 **JSON 路径**（`meshes[0].primitives[0].attributes.POSITION`），
/// 与 `scene3d_script` 同一口径：生成代码的模型拿到路径就能自己修。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/animation.hpp"
#include "st/raster/skin.hpp"
#include "st/raster/scene3d.hpp"

namespace st::raster {

/// 导入时一个网格实例（节点）的变换与材质。
struct GltfNode {
  std::string name{};
  math::Mat4 transform{math::Mat4::identity()};   ///< 已合成父链
  int material{-1};                               ///< 指向 `GltfScene::materials`；-1 = 默认材质
  std::vector<std::size_t> meshes{};              ///< 指向 `GltfScene::meshes`
};

/// glTF 导入结果（**已展开**成"可直接画的东西"）。
struct GltfScene {
  std::vector<Mesh> meshes{};
  struct MaterialSlot {
    math::Color base_color{255, 255, 255, 255};
    float metallic{0.0f};
    float roughness{1.0f};
    float emissive{0.0f};
    math::Vec3 emissive_color{1.0f, 1.0f, 1.0f};
    float opacity{1.0f};
    std::string name{};
  };
  std::vector<MaterialSlot> materials{};
  std::vector<GltfNode> nodes{};
  /// 文件里声明的相机（缺省时保留霜天默认相机）。
  bool has_camera{false};
  Camera camera{};
  /// 资产生成器（`asset.generator`），用于排查"这文件谁导的"。
  std::string generator{};

  /// 文件里的动画（**已翻译成本框架的 `Clip`**）。
  ///
  /// ## 翻译口径（三处必须对齐，否则"在 Blender 里对、在霜天里不对"）
  ///
  /// | glTF | 本框架 |
  /// |---|---|
  /// | `channels[].target.node` | 节点名（`nodes[].name`，无名则 `nodeN`） |
  /// | `path: translation` | `TrackKind::Translation` |
  /// | `path: rotation`（四元数采样） | `TrackKind::Rotation`（**转成欧拉角存**） |
  /// | `path: scale` | `TrackKind::Scale` |
  /// | `sampler.interpolation: LINEAR` | `Easing::Linear` |
  /// | `sampler.interpolation: STEP` | `Easing::Constant` |
  /// | `sampler.interpolation: CUBICSPLINE` | 取控制点（不支持切线的曲率，**声明为近似**） |
  ///
  /// ## 为什么不保留四元数采样
  ///
  /// 本框架的 `Keyframe::value` 是 `Vec3`（欧拉角）。转成欧拉角**是有损的**
  /// （万向锁附近），所以这里只做"能播出来"的翻译，不假装精确：
  /// 需要精确保真时请直接用 glTF 文件本身，不要经本结构往返。
  std::vector<Clip> animations{};

  /// 文件里的骨架（`skins[]`，最多取第一个）。
  ///
  /// ## 为什么只取一个
  ///
  /// 一个 glTF 可以有多个 skin（多个角色共享一份文件），但本框架的
  /// `SceneSpec` 只支持一副骨架（多角色需要多份场景）。取第一个并**如实声明**，
  /// 好过"看起来支持但实际只用了一个"。
  std::optional<Skeleton> skeleton{};

  /// 网格里是否带蒙皮权重（`JOINTS_0`/`WEIGHTS_0` 是否读到）。
  bool has_skin_weights{false};
};

/// 导出时的一个可写对象（几何 + 变换 + 材质）。
struct GltfObject {
  Mesh mesh{};
  std::string name{"mesh"};
  math::Mat4 transform{math::Mat4::identity()};
  Material material{};
  /// 该对象的网格是否带蒙皮权重（写 `JOINTS_0`/`WEIGHTS_0` + `skins[]`）。
  bool skinned{false};
  /// 节点层级：父节点在 `objects` 里的下标（-1 = 根）。
  ///
  /// 为什么用下标而不是名字：`GltfObject` 是**导出**用的临时结构，
  /// 调用方常常是"循环造一批对象"，名字可能重复或为空；
  /// 下标在同一批里永远唯一，且建树时不必再做一次名字查找。
  int parent{-1};
};

/// 导出选项。
struct GltfWriteOptions {
  /// 写 `.glb`（单文件二进制）还是 `.gltf`（JSON + 外部 `.bin`）。
  bool binary{false};
  /// `.gltf` 模式下把缓冲**内嵌成 base64**（不产生额外的 .bin 文件）。
  bool embed_binary{false};
  /// 外部 `.bin` 的文件名（`binary=false && embed_binary=false` 时用）。
  std::string binary_name{"buffer.bin"};
  /// 场景根节点名。
  std::string root_name{"World"};
  /// 写入 `asset.generator` 的字串（默认带框架名与版本）。
  std::string generator{};
  /// 是否写相机（`camera` 有效时）。
  bool write_camera{false};
  Camera camera{};
  /// 要写出的骨架（`objects[].skinned` 为真的对象会引用它）。
  std::optional<Skeleton> skeleton{};
  /// 要写出的动画（可多段）。每段的 `Track::node` 必须与某个 `GltfObject::name` 同名。
  ///
  /// 空 = 不写 `animations`（静态文件）。
  std::vector<Clip> animations{};

};

/// 从 glTF 文本（`.gltf`，JSON）或二进制（`.glb`）导入。
///
/// 自动识别：前四字节是 `glTF` 魔数 → 按 `.glb` 解析；否则按 JSON 解析。
[[nodiscard]] auto gltf_read(std::span<const std::uint8_t> bytes) -> Result<GltfScene>;

/// 读取文件（按扩展名与魔数双判；`.gltf` 的外部 `.bin` 会按相对路径自动查找）。
[[nodiscard]] auto gltf_read_file(std::string_view path) -> Result<GltfScene>;

/// 生成 `.glb` 二进制（单文件；可直接拖进 Blender / 引擎 / 网页查看器）。
[[nodiscard]] auto gltf_write_binary(const std::vector<GltfObject>& objects,
                                     const GltfWriteOptions& options) -> Result<std::vector<std::uint8_t>>;

/// 生成 `.gltf` JSON 文本。`binary_name` 非空且 `embed_binary=false` 时，
/// 还需要把 `--out-binary` 返回的字节写到同目录下同名文件（见 `GltfWriteResult`）。
struct GltfWriteResult {
  std::string json{};                        ///< `.gltf` 文本（`binary == true` 时为空）
  std::vector<std::uint8_t> binary{};        ///< `.glb` 整包，或 `.bin` 缓冲
  bool binary_is_glb{false};
  std::string binary_name{};                 ///< 需要与 JSON 一起写出的文件名（可空）
};

/// 生成 `.gltf`（JSON）+ 可选外部 `.bin`。
[[nodiscard]] auto gltf_write_json(const std::vector<GltfObject>& objects,
                                   const GltfWriteOptions& options) -> Result<GltfWriteResult>;

/// **一步写盘**：按 `options.binary` 决定写 `.glb` 还是 `.gltf` + `.bin`（自动建目录）。
[[nodiscard]] auto gltf_write_file(std::string_view path, const std::vector<GltfObject>& objects,
                                   const GltfWriteOptions& options) -> Status;

/// 把 glTF 场景**画进**三维场景（按节点变换与材质逐个 `draw_mesh`）。
///
/// 为何需要：导入之后调用方要自己遍历 nodes、套材质、算变换——那是三段容易写反的代码
/// （尤其实例化同一网格时材质会串）。这一个函数把正确顺序封装掉，
/// 与 `scene3d_script::render_scene` 是同一个设计取向。
[[nodiscard]] auto gltf_draw(const GltfScene& scene, Scene3D& target) -> Status;

}  // namespace st::raster
