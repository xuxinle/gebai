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
///   "lighting": { "light_from": [0.4, 0.8, 0.6], "ambient": 0.2,
///                 "diffuse": 0.6,
///                 "extra_lights": [{"direction": [-0.6,-0.2,0.5], "intensity": 0.5}] },
///   "shadow":  { "strength": 0.45, "ground_y": -0.55 },
///   "objects": [
///     { "shape": "sphere", "radius": 0.9, "color": "#c8a07a",
///       "metallic": 0.0, "roughness": 0.2, "specular": 1.0,
///       "translate": [0, 0.4, 0], "rotation": [0, 0, 0] }
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

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "st/raster/animation.hpp"
#include "st/raster/skin.hpp"

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
  /// 欧拉角（**度**），顺序 X→Y→Z。
  ///
  /// ## 为什么用「度」而不是「弧度」
  ///
  /// 旧名 `rotate` + 弧度，是模型最容易写错的一处：它几乎必然先写 90
  /// （角度直觉），而 90 弧度 ≈ 5156° —— 旋转看起来"完全乱了"，不报错。
  ///
  /// 现在名字是 `rotation`、单位是**度**：
  /// - 名字说明它是"姿态"而不是动词；
  /// - 单位与人的直觉一致（写 90 就是转 90 度）。
  ///
  /// 内部数学仍用弧度（`math::` 都吃弧度），在 `build_mesh`/节点求解的边界处转一次。
  math::Vec3 rotation{0.0f, 0.0f, 0.0f};
  math::Vec3 scale{1.0f, 1.0f, 1.0f};

  // ── 统一尺寸字段（2026-10-31，**首选写法**）──
  //
  // ## `size` 的语义：**全长**，顺序由形状决定
  //
  // 旧字段（`radius`/`width`/`height`/`depth`）的根本问题是
  // **同一名字在不同形状下含义不同**（实测：`plane` 的 `radius` 是半长、
  // `width` 是全长；`cube` 的 `height` 是 Z 向；`height` 对 `cylinder` 完全无效）。
  // 模型无法从名字推出语义，只能去读 2.6 万字的文档或赌一把。
  //
  // `size` 用**一个字段 + 按形状定顺序**解决：
  //
  // | 形状 | `size` 含义 |
  // |---|---|
  // | `cube` / `box` | `[X, Y, Z]` 三边全长 |
  // | `plane` | `[X, Z]`（厚度可选第三项） |
  // | 球类 | `直径`（标量） |
  // | 柱/锥/胶囊/棱柱/齿轮 | `[直径, 高]` |
  // | `torus` | `[外径, 管径]` |
  //
  // **一律是全长，没有半长**。模型只需记住"这是一整个东西有多长"，
  // 不必区分 radius/半宽/全长——那正是旧设计最容易出错的地方。
  //
  // `size_count == 0` 表示没写（走旧字段或默认值）。
  math::Vec3 size{0.0f, 0.0f, 0.0f};
  /// `size` 被写了几项（1、2 或 3）。0 = 未使用 `size`。
  std::uint8_t size_count{0};

  // 形状参数（按 shape 取用；语义与 `primitives::PrimitiveParams` 逐项一致）
  //
  // 尺寸**只有** `size` 一条路（见上）。旧字段 radius/width/height/depth
  // 已在 2026-10-31 彻底删除——它们在各形状下语义不同，是错误之源。
  int segments{24};
  /// 球的纬度环数（`uv_sphere`）。
  int rings{16};
  /// 二十面体球/正多面体的细分次数（`ico_sphere`）。
  int subdivisions{1};
  /// 环的主/小半径（`torus` / `torus_knot`）与截面细分。
  float major_radius{0.0f};   ///< 0 = 用 `radius`（即环外半径）
  float minor_radius{0.0f};   ///< 0 = 用 `major_radius * 0.35`（与旧 `torus` 的比例一致）
  int minor_segments{12};
  /// 锥的顶面半径（0 = 尖锥；>0 = 圆台）。
  float top_radius{0.0f};
  /// 网格平面的两向分段（`grid`）。
  int columns{8};
  int rows{8};
  /// 棱柱边数（非 0 时覆盖 `segments`）。
  int sides{0};
  /// 齿轮的齿数/齿高/齿宽占比；管的壁厚占比。
  int teeth{12};
  float tooth_height{0.08f};
  float tooth_width{0.5f};
  /// 扭转（弧度，整段高度上的总扭转）。
  float twist{0.0f};
  /// 螺旋的圈数与收缩；纽结的 (p, q)。
  float turns{3.0f};
  float taper{0.0f};
  /// 壁厚（beam 型钢 / fastener 紧固件；0 = 按比例取默认）。
  float thickness{0.0f};
  /// 截面/零件选型：beam 的 i/channel/angle/box、ornament 的 star/cross、
  /// fastener 的 bolt/nut/washer（空 = 该图元默认型）。
  std::string section{};
  int knot_p{2};
  int knot_q{3};
  /// 平面/盘/环带是否朝 +Z（立面），默认朝 +Y（地面）。
  bool face_camera{false};


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

  // ── 组合模型的空间体系（2026-10-30）──
  //
  // 一个"组合模型"= 一棵节点树。树上的每个节点有自己的**局部坐标系**，
  // 子节点相对父节点定位（父动子动）。这与 Blender 的父子关系、游戏引擎的
  // Scene Graph 是同一套语义，所以数值可以直接互相搬。
  //
  // ## 两种等价写法（都支持，选顺手的那个）
  //
  // 嵌套（层级清楚，适合人读）：
  // ```json
  // { "name": "robot", "children": [
  //     { "name": "arm", "translate": [0.5, 0.3, 0], "children": [
  //         { "shape": "capsule", "translate": [0, -0.4, 0] } ] } ] }
  // ```
  // 扁平（适合增量编辑：不必维护括号层级，改一行不影响其它行）：
  // ```json
  // [ { "name": "robot" }, { "name": "arm", "parent": "robot" },
  //   { "shape": "capsule", "parent": "arm", "translate": [0, -0.4, 0] } ]
  // ```
  //
  // ## 坐标系约定（与 Blender / glTF 一致）
  //
  // | 项 | 约定 |
  // |---|---|
  // | 手性 | 右手系，+Y 朝上 |
  // | 角度 | 弧度；`rotate` 的合成顺序 **X → Y → Z** |
  // | 局部 | 子节点的 `translate/rotate/scale` **相对父节点** |
  // | 枢轴 | 旋转与缩放**绕枢轴**，平移不受枢轴影响（= Blender 的 origin） |
  //
  // 旧脚本（扁平 `objects`、三个字段 translate/rotate/scale）**逐像素不变**：
  // 没有 `children` / `parent` / `pivot` 时，每个对象就是一棵只有根的树，
  // 世界矩阵 = 原来的 `T · R · S`。

  /// 节点名（**在同一场景内唯一**；按名字寻址、报错指路、导出后的引擎对象名都用它）。
  std::string name{};
  /// 父节点名（扁平写法用；与 `children` 互斥——给了 `parent` 就别再写嵌套）。
  std::string parent{};
  /// 子节点（嵌套写法用）。
  std::vector<SceneObjectSpec> children{};
  /// 是否是**分组节点**（`false` = 只承载子节点、自己不画几何）。
  ///
  /// 为什么需要它：给一组零件一个共同枢轴（"整台设备绕底座中心转"）时，
  /// 那个父节点本身没有几何。默认 `has_geometry = true` 以保持旧行为
  /// （旧对象都是叶子）。
  bool group{false};
  /// 枢轴点（**在自身局部坐标系里**）；旋转与缩放绕它进行。
  math::Vec3 pivot{0.0f, 0.0f, 0.0f};
  /// 枢轴预设（按自身几何包围盒算）：`origin` / `center` / `bottom` / `top`。
  /// 写了预设就以它为准（覆盖 `pivot` 的裸值），因为它通常比手算可靠。
  std::string pivot_at{};
  /// 是否可见（不可见不渲染、不参与包围盒）。
  bool visible{true};

  // ── 材质贴图（2026-10-30）──
  //
  // 采样的契约：**纹理 × 顶点色**（顶点色当"染色/基础色"，纹理提供图案）。
  // 这与 Blender 的 Base Color 纹理 + 顶点色相乘是同一套语义。
  //
  // ## 为什么 UV 变换放在这里而不是烘焙进网格
  //
  // "让同一张图在不同零件上以不同密度平铺"是极常见的需求（砖墙、地面、布料），
  // 而烘焙意味着每个零件一份网格副本（内存 × N、且不再共享）。
  // UV 变换只是一次仿射：`uv' = uv * scale + offset`——放在采样前做的成本是每像素两次乘法。
  /// 贴图路径（PNG；相对路径按 `asset_basis` 解析）。空 = 不用贴图。
  std::string texture{};
  /// UV 缩放（`u*scale_u`、`v*scale_v`）：把同一张图平铺多次时用（如 4,4 = 4×4 平铺）。
  ///
  /// **超出 0..1 会按 repeat 环绕**（贴图平铺的语义），而不是 clamp——
  /// clamp 会让"平铺"变成"边缘色拉伸"。两种行为用 `texture_clamp` 切换。
  float texture_scale_u{1.0f};
  float texture_scale_v{1.0f};
  /// UV 偏移（平铺时的相位）。
  float texture_offset_u{0.0f};
  float texture_offset_v{0.0f};
  /// 是否把超出 0..1 的 UV 夹到边缘（默认 `false` = repeat 平铺）。
  bool texture_clamp{false};
  /// 是否按贴图的 alpha 做透明（默认 `false`：PNG 的 alpha 常用于遮罩，不该默认吃掉）。
  bool texture_alpha{false};

  /// 是否参与蒙皮（`true` 且场景给了 `skeleton` 时，这个对象的网格会按骨骼形变）。
  ///
  /// 为什么是**每对象**的开关而不是全局：一个场景里常常既有蒙皮的软体
  ///（角色/布料）又有刚体（地板/道具）；给后者也蒙皮只会浪费 CPU。
  bool skin{false};

  /// 外部 **glTF** 引用（`gltf:<路径>` 形式时用；非空时覆盖 `shape` 的几何）。
  ///
  /// ## 为什么给 glTF 一个独立字段而不是又一种 `shape` 前缀
  ///
  /// `obj:` 前缀是历史形态（已经有一批脚本在用），而 glTF 需要**额外两件事**：
  /// ① 一份 glb 里可能有多个网格与节点变换（要全部摆进来）；
  /// ② 材质应当来自文件（PBR 参数），而不是对象上的 `metallic/roughness`。
  /// 用前缀会把这些差异藏进"形状名"，读代码的人看不出来。
  /// 所以单独一个字段：`shape` 继续管内置图元，`gltf` 管外部文件，语义互斥且显式。
  std::string gltf{};
  /// `gltf` 是否覆盖本对象的材质（默认 `true`：文件里写了材质就用文件的）。
  bool gltf_use_material{true};
  /// 是否作为**地面**（接收阴影、不遮挡阴影）。
  ///
  /// JSON 里没法表达「先画谁」这种顺序依赖，所以用一个字段显式区分，
  /// 由渲染器负责排顺序。
  bool ground{false};

  // ── 装配约束（2026-11-03，声明式：attach / connect / align）──
  //
  // 三个字段互斥（同时写两个是建模错误）；解算发生在解析后、建树前，
  // 结果写回 translate/rotation/size——下游零改动。语义与解算器见
  // `scene_constraints.hpp`（锚点语法、依赖规则、与 pivot_at 的分工都在那）。
  std::string attach{};
  std::string connect{};
  std::string align{};
  /// `connect` 时 size 的 Y 项是否自动（写 "auto"）——解算时填真实长度。
  bool connect_auto_length{false};

  /// 布尔运算类型（"union"/"subtract"/"intersect"；空 = 普通对象）。
  ///
  /// 非空时 `children` 的语义变为【操作体列表】（首个是基体）：
  /// `<Subtract><Box/><Cylinder/></Subtract>` = 立方体减圆柱。
  /// 子元素各自的 translate/rotation/scale 在网格层烘培后参与运算；
  /// 运算结果可以再套 size 缩放。详见 geometry.md 布尔一节。
  std::string op{};

  /// 草图轮廓（extrude/revolve 形状）：`"0,0 1,0 1,0.8 0.5,1.4 0,1.4"`
  ///（空格分点、逗号分坐标；逆时针或顺时针都收，生成器归一）。
  std::string profile{};

  /// 阵列（"ring"/"row"；空 = 普通对象）。
  ///
  /// 非空时 children 的语义变为【被复制的原型】：首个子元素被实例化
  /// `pattern_count` 次（其余子元素忽略——多个原型请各自建阵列）：
  /// - `ring`：绕 Y 轴均分一圈（pattern_radius 半径）；
  /// - `row`：沿 pattern_step 等距排列。
  /// 实例化后 merge 成单网格（一次绘制、可直接当布尔输入）。
  std::string pattern{};
  int pattern_count{6};
  float pattern_radius{1.0f};
  math::Vec3 pattern_step{0.2f, 0.0f, 0.0f};
};

/// 一段动画的**脚本层形态**说明（真正的数据结构是 `Clip`，见 `st/raster/animation.hpp`）。
///
/// ## JSON 形状（与大模型友好优先：字段名直白、时间就是秒）
///
/// ```json
/// "animation": {
///   "name": "wave",
///   "duration": 2.0,
///   "tracks": [
///     { "node": "shoulder", "kind": "rotation",
///       "keys": [ { "t": 0.0, "value": [0, 0, 0], "easing": "ease-in-out" },
///                 { "t": 1.0, "value": [0, 0, 0.8] },
///                 { "t": 2.0, "value": [0, 0, 0] } ] },
///     { "node": "hand", "kind": "visible",
///       "keys": [ { "t": 0.0, "value": [1, 0, 0] }, { "t": 1.5, "value": [0, 0, 0] } ] }
///   ]
/// }
/// ```
///
/// `keys[].t` 用 **`t`**（而不是 `time`）：少打三个字、且和数学写法一致；
/// 但两级都接受 `time`（不让人因为拼写而被拒）。
/// 完整场景描述。
struct SceneSpec {
  Camera camera{};
  Lighting lighting{};
  PlanarShadow shadow{};
  CullMode cull_mode{CullMode::Back};
  std::vector<SceneObjectSpec> objects{};

  /// `obj:` 形状的路径基准目录（UTF-8：绝对路径，或相对于进程工作目录）。
  ///
  /// 为何放在 spec 而不是 `SceneObjectSpec`：一份场景里的所有外部网格
  /// 几乎总在同一个素材目录下，逐对象重复声明只会漂移。
  /// 为空时 `obj:` 的路径按**原样**交给 `st::fs`（即相对当前工作目录）。
  std::string asset_basis{};

  /// 网格吸附步长（0 = 关闭，默认）。
  ///
  /// ## 为什么是「约束之前」的一步预处理而不是渲染时叠加
  ///
  /// 吸附服务的是**手写摆放**（把 -1.05 对到 -1.0）；约束解算产出的是
  /// 精确的装配值（台面顶 + 半高），再吸附只会破坏咬合。所以顺序固定：
  /// 先吸附手写的 translate，再解算约束（解算值覆盖/叠加不受影响）。
  /// 只吸 translate（rotation/scale 吸附会把 45° 吸到网格上，不是用户意图）。
  float snap{0.0f};

  /// 骨架（可选）：与 `objects` 里的节点名对应。给了骨架且对象标了 `skin` 时，
  /// 渲染会按**当前姿态**做逐顶点蒙皮（LBS）。
  ///
  /// JSON 形态：
  /// ```json
  /// "skeleton": {
  ///   "joints": ["root", "spine", "head"],
  ///   "inverse_bind": [[16 个数], [16 个数], [16 个数]]   // 可省略（默认单位阵）
  /// }
  /// ```
  ///
  /// `inverse_bind` 省略时按单位阵处理——那适合"网格就在关节空间里"的简单情形；
  /// 真实角色应当在绑定姿态下用 `inverse_bind_from_world(节点世界矩阵)` 生成。
  std::optional<st::raster::Skeleton> skeleton{};

  /// 场景自带的动画（最多一段；播放时刻由 `render_scene_at` 指定）。
  ///
  /// 为什么放在 `SceneSpec` 而不是 `SceneObjectSpec`：动画是**跨节点**的
  ///（一条轨道动一个节点，一段动画动整台设备），挂在单个对象上表达不了。
  std::optional<st::raster::Clip> animation{};

  // ── three.js 源码场景（2026-11-05） ──
  //
  // ## 为何需要第三条表达路径
  //
  // 前两条（`objects` 声明式 JSON、过程式 `Scene3D` 调用）对**人**够用，
  // 对**模型**不够——不是因为能力少，而是因为语料里没有：模型写
  // three.js 几乎一次就对，写本框架的 JSON 要反复试。
  //
  // 所以这一档不是"再包一层 API"，而是**换掉表达层**：源码里直接写
  // `new THREE.Mesh(new THREE.ExtrudeGeometry(shape, ...), ...)`，
  // 由 `three_runtime` 抽出几何/材质/相机/灯光再交给本框架渲染。
  //
  // ## 与 `objects` 的关系：**互斥，非空时优先**
  //
  // 两者同时给会让“到底以哪个为准”变成隐规则。这里定死：
  // `three_script` 非空 ⇒ 忽略 `objects`；为空 ⇒ 完全走旧路径（逐像素不变）。
  //
  // ## 能写什么
  //
  // 完整 JS：循环、函数、条件。这是它相对声明式 JSON 的**核心优势**
  //（"沿弧线摆 24 个方块"在 JSON 里要写 24 遍，这里一个 for）。
  /// three.js 源码（非空时覆盖 `objects` 的几何描述）。
  ///
  /// 契约（与 `three_runtime` 一致）：最后一个表达式应是
  /// `THREE.Scene` / `Group` / `Mesh`，或定义一个名为 `scene` 的全局变量。
  std::string three_script{};

  /// three.js 源码在报错/日志里的**文件名**（默认 `<inline-three>`）。
  ///
  /// 为何可配：出问题时错误消息会带这个文件名与行号。从文件加载时填上真实路径，
  /// 模型/人才能一眼定位到源文件的那一行。
  std::string three_script_name{};

  // ────────────────────────────────────────────────────────────────
  // bpy 源码场景（2026-11-05）
  // ────────────────────────────────────────────────────────────────
  //
  // ## 为什么要第三条表达层
  //
  // `objects`（JSON）语料稀薄，`three_script` 在“一般三维场景”上很好，
  // 但**精确建模**（齿轮齿形、涡轮叶片、程序化几何）上不如 bpy：
  // 模型的语料里 Blender 那套（`bmesh` / `bpy.ops.primitive_*` /
  // `mathutils`）在机械件与资产级建模上远厚。
  //
  // ## 与另两条的关系：三者组合，不再互斥
  //
  // | 字段 | 作用 |
  // |---|---|
  // | `bpy_script` | 建模：产几何。**同时写 `three_script` 时变成“资产供应商”** |
  // | `three_script` | 组装/演示：最终场景以它为准（拿得到 `THREE.__assets`） |
  // | `objects` | 两者都空时的声明式路径 |
  //
  // ## 为何从“互斥”改成“叠加”（本轮的判断）
  //
  // 旧版把两个脚本定死为互斥，理由是“两个场景各带相机/灯光，谁生效说不清”。
  // 但真实工作流里它们不是两个场景，而是**同一场景的两个阶段**：
  // bpy 管几何（精确建模型），three.js 管布局与表现（摆位、材质变体、相机、动画）。
  // 于是现在的语义是**确定的**：
  //
  // 1. 先跑 bpy，把它的网格导出成 `THREE.__assets`（名字 = bpy 物体名）；
  // 2. 再跑 three.js，它用 `new THREE.Mesh(THREE.__assets.gear, mat)` 拿几何；
  // 3. **相机/灯光/物体全部以 three.js 的结果为准**（bpy 那边只取几何）。
  //
  // 第 3 条是关键：不必再猜“谁生效”——three.js 是最终场景的**唯一权威**，
  // bpy 降级为“几何提供者”。所以“两套相机冲突”这个问题在叠加模式下不存在。
  //
  // 只写 `bpy_script` 时行为完全不变（资产模式不参与，bpy 自己就是完整场景）。
  /// bpy 源码。
  ///
  /// 契约：脚本里把要导出的东西赋给 `result`（可选）；不写就导出场景里
  /// 全部带 mesh 的对象。
  ///
  /// 与 `three_script` **同时给**时：本脚本只提供几何（每个带 mesh 的 bpy 物体
  /// → `THREE.__assets.<物体名>`），相机/灯光/其它一切以 `three_script` 为准。
  std::string bpy_script{};

  /// bpy 源码在报错里的**文件名**（默认 `<inline-bpy>`）。
  std::string bpy_script_name{};

  // ── 叠加层（只对 `three_script` 生效）──
  //
  // ## 为何需要“叠加”而不是“覆盖”
  //
  // 场景脚本描述的是**模型本身**（几何 + 材质 + 它的固有相机）；
  // 而片子层还有一类参数与模型无关、只关乎“这个镜头里怎么用它”：
  // 转多少角度、浮多高、推拉多少。让每个场景脚本各写一份是重复劳动。
  //
  // 所以这里开三扇窗，**默认关闭**（保持“场景自己的表达优先”）：
  //
  // | 字段 | 作用 |
  // |---|---|
  // | `extra_transform` | 乘在每个物体世界矩阵**前面**（整体转/缩放/浮） |
  // | `camera_override` | 换掉场景自带相机（镜头推拉/机位切换） |
  // | `material_override` | 换掉逐物体材质（统一成一种质感） |

  /// 场景**整体**的额外变换：`extra_transform * object.world`。
  ///
  /// 语义是“把这个模型当成一个整体去摆弄”——绕原点转、整体缩放、上下浮。
  /// 与逐物体动画（`userData.spin`）不冲突：那是模型内部的运动，这是镜头级的编排。
  ///
  /// ⚠ 必须是 `pose * world` 而不是 `world * pose`：后者会让每个物体
  /// **各自绕自身原点**转，一个由多零件组成的模型会当场散开。
  std::optional<math::Mat4> extra_transform{};

  /// 换掉场景自带相机（`std::nullopt` = 用场景自己的）。
  std::optional<Camera> camera_override{};

  /// 换掉逐物体材质（`std::nullopt` = 用场景自己的）。
  ///
  /// 用处：“同一份模型，在这个镜头里是黄铜、在另一个里是磨砂黑”。
  std::optional<Material> material_override{};
};

/// 把一个形状引用解析成实际可加载的路径。
///
/// 解析顺序与裁云的 `resolve_obj_path` **同一套语义**：
/// 绝对路径 → 相对 `asset_basis` → 原样。第二档要求文件**真的存在**才采用，
/// 否则退回原样并把错误留给加载器报告（这样错信息里带的是调用方写的路径，
/// 而不是一个拼接出来的、看着陌生又不存在的路径）。
[[nodiscard]] auto resolve_shape_path(std::string_view reference, std::string_view asset_basis)
    -> std::string;

/// three.js 源码 ⇒ 可直接渲染的 `SceneSpec`（几何/材质/相机/灯光全部抽好）。
///
/// ## 为何要这个而不是让调用方自己调 `ThreeRuntime`
///
/// 调用方关心的是"这个场景要画什么"，不是"怎么从 JS 堆里把 Float32Array 搬出来"。
/// 本函数把那段搬运（以及它自带的**一堆语义翻译坑**：灯方向取反、side → 剔除模式、
/// three 的线性色 ↔ 本框架的线性色）收进一处——那几处每一处错了都是
/// “画面不对但不报错”。
///
/// ## 抽出来的物体长什么样
///
/// 每个物体带 `<three:N>` 形式的 `shape`（`N` 是 `three_primitives` 里的下标），
/// 几何**已经建好**，不再按 `shape` 参数重新生成。`render_scene*` 家族认这个前缀。
///
/// 错误：`Unsupported`（three 运行时不可用）、`Parse`（语法/运行时错误，带行号）、
/// `Timeout`（超过执行时限）、`Invalid`（脚本未产出场景）。
[[nodiscard]] auto scene_spec_from_three(std::string_view script,
                                         std::string_view script_name = "<inline-three>")
    -> Result<SceneSpec>;

/// 把 **bpy 源码**抽成 `SceneSpec`（Blender 的 Python 建模 API）。
///
/// ## 与 `scene_spec_from_three` 的关系
///
/// 两者**同构**：都是“跑脚本 → 抽 `ThreeScene` → 填 `SceneSpec` 的相机/光照”，
/// 区别只在于用哪个运行时。所以下游（`render_scene*`）对它们一视同仁。
///
/// ## 能力与边界
///
/// 子集覆盖 `mathutils` / `bpy.data` / `bpy.ops.mesh.primitive_*` / `bmesh`。
/// 已知硬限制：
///
/// · **`@` 运算符不可用**（MicroPython 编译器不支持 PEP 465），用
///   `.matmul()` / `.dot()` 代替
/// · 面三角化用**扇形**：凸多边形正确，凹多边形会出错
/// · 多材质对象只取第 0 个槽
///
/// 详见 `docs/python.md`。
///
/// 错误：`Unsupported`（MicroPython 不可用）、`Invalid`（Python 语法/运行时错，
/// 带异常类型与消息）、`Parse`（抽取结果不合法）。
[[nodiscard]] auto scene_spec_from_bpy(std::string_view script,
                                       std::string_view script_name = "<inline-bpy>")
    -> Result<SceneSpec>;

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
[[nodiscard]] auto render_scene(const SceneSpec& spec, Surface& canvas,
                                math::Color background) -> Result<void>;

/// 渲染**某一时刻**的一帧（场景自带动画时用它）。
///
/// ## 为什么给独立函数而不是给 `render_scene` 加个默认参数
///
/// "静态场景"与"动画场景"是两种用法：前者一次出图，后者逐帧出片。
/// 把 `time` 塞进 `render_scene` 会让静态调用点多一个无意义参数，
/// 也让"这份脚本到底有没有动画"变成运行期才知道的事。
///
/// ## 时间语义
///
/// - `time` 单位是**秒**；范围外按端点保持（不循环——循环是播放器策略，
///   见 `animation.hpp`）。要循环：`time = fmod(time, clip.duration())`。
/// - 没有动画时与 `render_scene` **逐像素相同**。
[[nodiscard]] auto render_scene_at(const SceneSpec& spec, float time, Surface& canvas,
                                   math::Color background) -> Result<void>;

/// 场景树里**会被绘制**的节点名（按先序；不含被跳过的）。
///
/// 用途是验证「哪些对象真的进了场景树」而不依赖出图像素：典型场景是
/// **布尔/阵列的操作体**——它们只是运算输入，几何已经烤进父对象的结果
/// 网格，不该再当独立实体画一次。
[[nodiscard]] auto collect_drawable_node_names(const SceneSpec& spec)
    -> std::vector<std::string>;

/// 同上，但复用调用方已有的场景实例（裁云导出时用这条）。
[[nodiscard]] auto render_scene_at_into(const SceneSpec& spec, float time, Scene3D& scene,
                                        Surface& canvas, math::Color background) -> Result<void>;

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
[[nodiscard]] auto render_scene_into(const SceneSpec& spec, Scene3D& scene, Surface& canvas,
                                     math::Color background) -> Result<void>;

/// 生成一个对象的网格（`shape` + 形状参数）。
///
/// 暴露出来是为了让调用方能单独检查"我的参数生成了什么网格"，
/// 而不是只能渲一整帧再看。
[[nodiscard]] auto build_mesh(const SceneObjectSpec& object) -> Result<Mesh>;

}  // namespace st::raster
