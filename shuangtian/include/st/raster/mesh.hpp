#pragma once

/// 三维网格：**平台无关**的几何数据与生成/加载。
///
/// 为什么它不在 `gl.hpp` / `gpu.hpp` 里：网格是纯数据 + 纯解析（顶点、索引、OBJ 文本），
/// 与"用哪个图形 API 画"毫无关系。它原先住在 `raster::gl` 里，导致：
/// ① 换后端（GL → D3D11）就得把整套解析搬一遍；
/// ② 非 Windows 平台上这份能力**整个消失**（`gl` 是 Windows 专用的）；
/// ③ OBJ 加载不得不依赖 GL 模块才能编译。
/// 抽到这里之后，任何后端、任何平台都能用它——几何与传输方式彻底分开。
///
/// 顶点的内存布局在 `kVertexFloats` 处定义一次，所有后端共用：
/// 布局是"后端上传缓冲"与"生成器产出"之间的契约，写两遍必然漂移。

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"

namespace st::raster {

/// 网格：交错顶点（位置 3 + 法线 3 + 顶点色 3）**加上可选的 UV 边表**。
///
/// ## UV 为何不放进顶点布局
///
/// `kVertexFloats = 9` 是**后端上传与生成器之间的契约**（`platform_d3d11.cpp` 与
/// `software_scene.cpp` 都按它算 stride）。把 UV 塞进去等于改 stride：
/// 所有后端、所有生成器、所有 OBJ 加载路径同时要改，而它们绝大多数
/// **根本不需要贴图**（纯光照模型已经够用）。
///
/// 所以 UV 存成一张**平行于顶点的边表**（每顶点 2 个 float，与顶点索引对齐）：
/// 只有真要用纹理的网格才填。`uvs` 为空就是“这个网格没 UV”，两条腿都退回顶点色。
///
/// 好处：零破坏（老代码一字不改）、零开销（不贴图不分配）、语义清楚
/// （“有没有 UV”是查询一次就知的事实，而不是一句口头约定）。
struct Mesh {
  /// 每顶点分量数（位置 3 + 法线 3 + 颜色 3）。后端上传缓冲按它算 stride。
  static constexpr std::size_t kVertexFloats = 9U;

  std::vector<float> vertices{};   ///< 每顶点 `kVertexFloats` 个 float
  std::vector<std::uint32_t> indices{};
  /// 每顶点 `(u, v)`（`u*N = v*N = uvs.size()/2`）；空 = 无 UV（不贴图）。
  std::vector<float> uvs{};

  [[nodiscard]] auto vertex_count() const noexcept -> std::size_t {
    return vertices.size() / kVertexFloats;
  }
  /// 是否有可用 UV（边长等于顶点数）。
  [[nodiscard]] auto has_uvs() const noexcept -> bool {
    return !uvs.empty() && uvs.size() == vertex_count() * 2U;
  }
  /// 第 `index` 个顶点的 `(u, v)`；无 UV 或越界返回 `(0, 0)`。
  [[nodiscard]] auto uv_at(std::size_t index) const noexcept -> std::pair<float, float> {
    if (!has_uvs() || index * 2U + 1U >= uvs.size()) return {0.0f, 0.0f};
    return {uvs[index * 2U], uvs[index * 2U + 1U]};
  }
  [[nodiscard]] auto is_empty() const noexcept -> bool {
    return vertices.empty() || indices.empty();
  }

  /// 第 `index` 个顶点的 9 个分量（位置 3 + 法线 3 + 颜色 3）。
  /// 只读访问供测试与导出用；写一律走 `vertices`（避免两套修改路径）。
  [[nodiscard]] auto operator[](std::size_t index) const -> std::span<const float> {
    const std::size_t offset = index * kVertexFloats;
    if (offset + kVertexFloats > vertices.size()) return {};
    return std::span<const float>(vertices.data() + offset, kVertexFloats);
  }

  /// 立方体（边长 `size`，各面不同色：一眼能看出朝向）。
  [[nodiscard]] static auto cube(float size = 1.0f) -> Mesh;
  /// 球（经纬网格，`segments` 越大越圆）。
  [[nodiscard]] static auto sphere(float radius = 0.6f, int segments = 24) -> Mesh;
  /// 长方体（三轴独立边长）。
  [[nodiscard]] static auto box(float x, float y, float z) -> Mesh;

  // —— 统一上色的生成器（2026-10-22）——
  //
  // 为何要加这一组：上面三个生成器给的是**六面异色**——那是为了“能看出朝向”
  // 而做的调试配色（单色立方体转过 90° 看不出来，会漏掉模型矩阵的错误）。
  // 但裁云要的是**产品主视觉**：一个统一色、带垂直渐变的盒体，而不是五彩魔方。
  //
  // 所以这组生成器把颜色作为参数，并在竖直方向做**浅渐变**（上亮下暗）：
  // 单一纯色 + Lambert 光照在平面侧壁上会显得像“贴片”，加一层方向性渐变
  // 才有“实体”的观感。渐变幅度刻意做小（±12%），否则会变成“上白下黑”。

  /// 四边倒角的盒体（统一色 + 竖直微渐变）。`bevel` 为**预留参数**：
  ///
  /// 当前实现**不做真倒角**（`bevel` 被忽略）——两种廉价近似都会在棱角留出
  /// 可见缝隙（详见 `mesh.cpp` 的实现注释），而直角盒体在本渲染器里已经干净。
  /// 参数保留是为了将来若真做几何倒角（26 面拆解）时不必改调用方。
  [[nodiscard]] static auto colored_box(float x, float y, float z, math::Color color,
                                       float bevel = 0.0f) -> Mesh;
  /// 统一色的球（`segments` 越大越圆）。
  [[nodiscard]] static auto colored_sphere(float radius, int segments, math::Color color) -> Mesh;
  /// 圆环（甜甜圈）：产品展示里最常用的“光环”形状。
  ///
  /// `major_segments`/`minor_segments` 控制平滑度（24×12 在 720p 下已看不出棱）。
  [[nodiscard]] static auto torus(float major_radius, float minor_radius, int major_segments,
                                  int minor_segments, math::Color color) -> Mesh;
  /// 圆柱（带顶/底盖）。
  [[nodiscard]] static auto cylinder(float radius, float height, int segments,
                                     math::Color color) -> Mesh;
  /// 平面（朝 +Z；可做“镜子/地面/投影底板”）。
  [[nodiscard]] static auto plane(float width, float height, math::Color color) -> Mesh;

  /// **把网格压到 Y=0 平面（投影/扁影用）**。
  ///
  /// ## 用途：接触阴影（“落地感”的来源，见 docs/visual_craft.md §1.1）
  ///
  /// 把物体的轮廓压成一片扁平形状，贴在地面上、用低透明度画一遍——它提供的
  /// 不是光学正确的阴影，而是**“这个物体碰到了地面”这个信号**。
  /// 实测：没有它，三维物体无论多边形再多都判“像贴纸”（物体与地面亮度接近，
  /// 大脑无法判断接触关系）；有了它，同一个模型立刻“站住”了。
  ///
  /// ## ⚠ 压平必须**去重**（踩过，必看）
  ///
  /// 直接把实体盒体压平会把 **6 个面全部塌到同一高度**：顶面、底面、四个侧面
  /// 变成一堆**互相重叠的共面多边形**。它们深度值完全相同，z-buffer 里互相
  /// 争夺（先画的赢，其余被丢弃），最终结果是“画了但看不见”。
  /// 实测代价：两轮排查都没有怀疑几何——因为 `draw_calls` 在涨、颜色也对，
  /// 根本看不出“提交了但被自己遮掉”。
  ///
  /// 所以这里只保留 **投影面积最大、法线朝上那一个面**（即立方体的顶面），
  /// 把它压到 `offset_y`。对盒体/平板类形状，它的轮廓就是正确的阴影形状。
  ///
  /// · `offset_y`：影子在 Y=0 平面上的高度（0 即可，抬高由调用方缩放后平移）。
  /// · `expand`：沿 X/Z 向外放大一点，模拟半影（真实阴影在接触处不是锐边）。
  ///
  /// 返回的网格**不带 UV**（它只用纯色，不需要贴图）。
  [[nodiscard]] auto flattened(float offset_y = 0.0f, float expand = 1.04f) const -> Mesh;

  /// **带 UV 的平面**（朝 +Z，UV 铺满 0..1）：贴图的最小载体。
  /// 图标的“面板/壁纸”类画面用得到；也是纹理管线的对照基准。
  [[nodiscard]] static auto textured_plane(float width, float height, math::Color tint) -> Mesh;

  /// **带 UV 的盒体**（六个面各自铺满 0..1）。
  ///
  /// 为何六面用同一套 UV：图标贴图通常是“六面同一个图案”（骰子/应用图标方块）。
  /// 需要分面不同图案时要多次绘制 + 换纹理（当前接口不支持单网格多材质）。
  [[nodiscard]] static auto textured_box(float x, float y, float z, math::Color tint) -> Mesh;

  /// **贴图平板**：厚度方向是 Y，**顶面（+Y）贴图**，其余面用纯色（侧壁）。
  ///
  /// 用途：把一张二维画面（桌面截图、产品界面、海报）当成一块**有厚度的立体块**
  /// 摆进三维场景——那才能看到透视与边缘厚度，而不是“背景贴图”。
  ///
  /// 尺寸参数：`width` = X 向宽，`thickness` = Y 向厚，`depth` = Z 向深。
  ///
  /// ## 顶面 UV 的方向约定
  ///
  /// 从上方俯视时，纹理的 **v=0 在 -Z 侧（“远端”）**，即“画面的上方对着场景深处”——
  /// 这才是“桌面平铺在地上”的正确观感。若反过来，画面会变成“朝着观众躺倒”。
  ///
  /// ## 侧壁 UV 为何钉在一个很小的常量坐标
  ///
  /// 网格级的 `has_uvs()` 是“全体顶点都带 UV”，所以侧壁顶点也必须给 UV（否则
  /// 会采样到 garbage）。这里把它们钉到 `(kSideU, kSideV)`——一个**约定好的角落**，
  /// 纹理生成器会在那里铺一小块纯侧壁色，于是双线性采样落在均匀色区域内、不会串色。
  /// `kSideU/kSideV` 公开成常量，保证“网格”与“纹理生成”两边用同一个坐标。
  /// **图标立方体**：只有 **+Z 正面**贴图，其余五面纯色。
  ///
  /// ## 为何不能用 `textured_box`
  ///
  /// `textured_box` 六面都铺满 0..1 UV，于是**侧面也印着图标的一部分**。
  /// 在透视下侧面可见，观感上就像“图标被切成几块贴在各个面上”——实测被误判为
  /// “多了一个半透明块”。真实的应用图标在桌面上只有正面有图案，侧面是该应用的主题色。
  ///
  /// 侧面的 UV 钉在 `kSideU/kSideV`（贴图角落的纯色区），
  /// 与 `textured_slab` 同一套机制——纹理生成时会在那里埋一块侧面色。
  [[nodiscard]] static auto textured_icon_box(float x, float y, float z, math::Color side_color)
      -> Mesh;

  /// 侧壁 UV 坐标（钉在贴图左上角的纯侧壁色带内）。
  ///
  /// ## 数值为何这么小、为何必须与 `kSideGutter` 联动
  ///
  /// 角带宽度 = `kSideGutter` 像素 / 贴图宽。512 贴图 + gutter=4 → 占 0.0078。
  /// 而 `kSideU/kSideV` 必须**深在角带内部**，且留出双线性的混合半径
  /// （采样会混合相邻 texel，靠边就会串色）。0.002 × 512 ≈ 1 px，安全。
  ///
  /// ⚠ `kTopInset` 必须**明显大于**角带宽度：早先两者取同一量级（0.008 vs 0.0078），
  /// 正面 UV 最左侧恰好在角带边界上，双线性一混就把侧面深色抹进了图标边缘，
  /// 整块图标看起来“发暗/消失”。现在 `kTopInset` 取 3 倍余量。
  static constexpr float kSideU = 0.003f;
  static constexpr float kSideV = 0.003f;
  /// 贴图 UV 的内缩量（避开左上角那条侧壁色带）。
  static constexpr float kTopInset = 0.008f;
  [[nodiscard]] static auto textured_slab(float width, float thickness, float depth,
                                         math::Color side_color) -> Mesh;

  /// 改所有顶点的颜色（保持坐标与法线不变）。
  ///
  /// 用途：同一份几何要在不同镜头里换色/换品牌色，而重新生成网格（球是几千个顶点）
  /// 不如改颜色便宜。返回新网格而不是就地改：网格常被多个场景共享，就近改会
  /// 让“上一镜的颜色”渗透到下一镜（且这种缺陷只在特定渲染顺序下出现）。
  [[nodiscard]] auto tinted(math::Color color) const -> Mesh;

  /// 从 **OBJ 文本**加载网格（Wavefront OBJ，只取几何：`v` / `vn` / `f`）。
  ///
  /// 支持：`f` 的四种写法（`v`、`v/vt`、`v//vn`、`v/vt/vn`）、**负索引**（相对引用）、
  /// 多边形面（扇形三角化）、缺法线时**按面计算**。
  /// 不支持（如实说明）：`vt` 纹理坐标、材质库（`mtllib`/`usemtl`）、自由曲面、多对象分组
  /// —— 取到就跳过，而不是猜。这不是"完整的 OBJ 实现"，是"够画出模型的子集"。
  ///
  /// @return 失败：空中/无有效面（`Invalid`）。**单个坏面只跳过**，不让整个文件失败。
  [[nodiscard]] static auto load_obj(std::string_view text, math::Color base_color)
      -> Result<Mesh>;

  /// 从文件加载（路径按 UTF-8 处理，经 `st::fs`——Windows 下中文路径才不会坏）。
  [[nodiscard]] static auto load_obj_file(std::string_view path, math::Color base_color)
      -> Result<Mesh>;
};

/// 把单个顶点写入交错缓冲（生成器与 OBJ 加载共用，保证布局只有一处定义）。
void push_vertex(std::vector<float>& out, math::Vec3 position, math::Vec3 normal,
                 math::Vec3 color);

}  // namespace st::raster
