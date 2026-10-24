#pragma once

/// 三维场景：**平台中立的接口**，由当前平台的最佳图形 API 实现。
///
/// 为什么接口在这里，而不是在某个具体后端里（例如原先的 `raster::gl`）：
/// "把三维画进一块区域"是框架能力，**与用哪个 API 无关**。它原先定义在 `gl.hpp` 里，
/// 于是这个能力被绑死在 OpenGL 上——换后端要重写调用方，非 Windows 平台直接没有。
/// 现在：接口中立、实现按平台选（Windows 用 D3D11），调用方（`ui::GlView`）不感知后端。
///
/// 与 2D 的关系：这不是"另一个 2D 后端"，而是**补上 2D 做不到的事**
/// （透视投影、深度缓冲、逐像素光照、任意网格）。2D 界面仍走 `Surface`；
/// 需要真三维的地方调这里，渲染结果再合成回画布。

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "st/core/error.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/color_pipeline.hpp"
#include "st/raster/mesh.hpp"

namespace st::raster {

class Surface;

/// 面朝向剔除模式。
///
/// ## 为何需要（实测）
///
/// 不加剔除时，闭合物体的**背向面也会画**。对不透明封闭体它们会被深度测试
/// 挡掉（结果碰巧正确），但两种场合会直接穿帮：
/// - 相机钻进物体内部时，看到的是内壁（本应一片黑）；
/// - **透明物体**：背面不剔除时，后面与前面两个面都会被混合，
///   颜色叠加两次——“玻璃变浓”就是这么来的。
///
/// 默认值 `Back` 与 Blender/游戏引擎一致。
/// 单面物体（如 `Mesh::plane`）从背面看会消失，这也是标准行为。
enum class CullMode : std::uint8_t {
  None = 0,   ///< 不剔除（双面可见；给“薄片双面”场景用）
  Back = 1,   ///< 剔除背向面（**默认**，封闭物体的正确行为）
  Front = 2,  ///< 剔除正向面（看内壁时用；不常见）
};

/// 材质参数。与 `set_texture` 同一设计：**材质是场景的一个状态**，
/// 不是网格的一部分（原因见 `set_texture` 的说明——换材质不应重建网格缓存）。
struct Material {
  /// 不透明度（1 = 完全不透明，0 = 完全透明）。
  ///
  /// 小于 1 时，网格的三角形会被推到**透明队列**：
  /// 先画完全部不透明物体，再按“远→近”排序后混合。
  /// 这是在没有完整 BSP 排序的前提下能做到的正确做法。
  float opacity{1.0f};
  /// 是否写入深度。透明物体必须关（否则后面的透明面会被它挡住）。
  /// 一般不需要手动改：`opacity < 1` 时自动为 false。
  bool depth_write{true};
};

/// 相机（右手系，视线朝 -Z）。
struct Camera {
  math::Vec3 eye{0.0f, 0.0f, 4.0f};
  math::Vec3 target{0.0f, 0.0f, 0.0f};
  math::Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_y_degrees{45.0f};
  float near_z{0.1f};
  float far_z{100.0f};
};

/// ## 色彩空间口径（**读一次能省一堆调试**）
///
/// 三维管线默认在**线性空间**算光照、最后经色调映射 + sRGB 编码输出。
/// 于是不同入口的输入口径**不一样**，传错就会“画面亮一档但看不出原因”：
///
/// | 入口 | 该传什么 | 为什么 |
/// |---|---|---|
/// | `Mesh::colored_sphere` / `colored_box` / `plane` / `textured_*` 的 `color` | **sRGB 字节** | 与设计工具里看到的色一致；内部解码 |
/// | `Mesh::vertices` 直接写 | **线性值** | 那是已经解码后的工作空间 |
/// | `set_texture()` 的 RGBA8 | **sRGB 字节** | 那是 PNG/图片文件里的字节 |
/// | `begin_frame(clear_color)` | **线性值** | 与 `Lighting` 同一口径 |
/// | `Lighting` 里的颜色（隐含于 `key_direction` 等） | **线性值** | — |
/// | `Lighting::exposure` / `tone_map` | 曝光 / 算子 | 作用于**线性** HDR 值 |
///
/// 一句话：**从图片/设计工具来的 → sRGB；在代码里手算的 → 线性**。

/// 光照参数。**默认值就是历史行为**（键光方向、环境 0.35、边缘光 0.25、漫反射 0.75）。
///
/// 为何要参数化：软件腿原先把这三组数字写死在 `shade()` 里，于是"换一个打光"
/// 成了改代码重编译。裁云要把三维当画面主体用，打光就是**构图的一部分**——
/// 同一个模型，正面平光与侧逆光完全是两张片子。
///
/// 两条腿（软件 / GPU）必须消费**同一组**参数与**同一个公式**（见 `shade_terms`），
/// 否则"降级"就不是降级，而是换了一种长相。
struct Lighting {
  /// 主光方向（世界空间，**指向光源**，不需要预先归一化）。
  math::Vec3 key_direction{0.4f, 0.8f, 0.6f};
  /// 环境项：无光照处的底色亮度（0 = 背光面全黑）。
  float ambient{0.35f};
  /// 漫反射权重（与环境项相加，1.10 ≈ 原值 ambient 0.35 + diffuse 0.75）。
  float diffuse{0.75f};
  /// 边缘光（轮廓光）强度：让模型从背景里"浮"出来。
  float rim{0.25f};
  /// 整体曝光乘子（1 = 不调）。批量出片时统一压/提亮比逐个改顶点色省事。
  float exposure{1.0f};
  /// 色调映射算子（见 `color_pipeline.hpp`）。
  ///
  /// **默认 `Aces`**：它是“正确”的那一档（HDR 滚降、高光不糊），
  /// 而且现有场景全部在 [0,1] 内，ACES 对中调的影响是“略提亮”——
  /// 实测 0.5 → 0.616，画面变得更接近预期。
  ///
  /// 要逐像素回到旧行为（无映射、线性值直接当 sRGB），把它设成 `None`；
  /// 但**不推荐**——那是把“线性当 sRGB”的错误固化下来。
  ///
  /// 注：`color_pipeline.hpp` 里的 `ToneMap` 定义在 `st::raster`，
  /// 本结构也在同一命名空间，所以这里直接写类型名。
  ToneMap tone_map{ToneMap::Aces};
};

/// 一像素的光照结果（两条腿共用同一套算式的**唯一**定义处）。
///
/// 存在的理由：软件腿逐像素调它、GPU 腿的 HLSL 是它的**逐字翻译**。
/// 把算式写成一个具名函数，两边的"看起来差不多"才能被测试钉住
/// （`raster_scene3d_lighting_test` 用同一组输入比两边输出）。
[[nodiscard]] auto shade_terms(math::Vec3 normal, math::Vec3 color, const Lighting& lighting)
    -> math::Vec3;

/// 三维场景交出的**原生纹理句柄**（供同后端的上层零拷贝合成）。
///
/// 定义在 `Scene3D` 上而不是 `Surface`：三维场景**不是**一张画布（它没有
/// fill_rect/文字/裁剪这些能力），只是“一块渲好的纹理”。把两者分开，
/// `Surface` 就不必为一个它多数实现用不上的能力承担接口重量。
struct SceneTexture {
  const void* handle{nullptr};   ///< 后端自有对象（D3D11 上是 `ID3D11ShaderResourceView*`）
  const char* backend{nullptr};  ///< 后端标签（如 `"d3d11"`）；不匹配即不可用
  int width{0};
  int height{0};

  [[nodiscard]] auto valid() const noexcept -> bool { return handle != nullptr; }
};

/// 三维场景：离屏渲染 → 合成进画布。
///
/// 用法：`create` → 每帧 `begin_frame` / `set_camera` / `draw_mesh(…)` / `end_frame(surface, dest)`。
/// 一次 `begin_frame` 内的所有 `draw_mesh` 共享同一深度缓冲（因此**互相遮挡关系正确**）。
class Scene3D {
 public:
  Scene3D() = default;
  virtual ~Scene3D() = default;
  Scene3D(const Scene3D&) = delete;
  auto operator=(const Scene3D&) -> Scene3D& = delete;

  /// 本平台是否有可用的三维实现（编进来 **且** 运行期可用）。
  [[nodiscard]] static auto available() noexcept -> bool;

  /// 建离屏场景（尺寸为**物理像素**）。失败给出具体原因（无设备/资源创建失败/着色器编译错误）。
  [[nodiscard]] static auto create(int width, int height) -> Result<std::unique_ptr<Scene3D>>;

  /// 开始一帧：设视口、清色与深度。
  virtual void begin_frame(math::Color clear_color) = 0;
  virtual void set_camera(const Camera& camera) = 0;
  /// 设光照（**不调也可**：`Lighting` 的默认值就是历史行为）。每帧重置为默认。
  virtual void set_lighting(const Lighting& lighting) = 0;

  /// 设面朝向剔除模式（**不调也可**：默认就是 `CullMode::Back`）。每帧重置为默认。
  ///
  /// 为何要每帧重置：与 `Lighting` 同理——不重置会让上一帧的设置泄漏到下一帧，
  /// 而那种“某个场景之后全都错了”的 bug 极难定位。
  virtual void set_cull_mode(CullMode /*mode*/) {}

  /// 设材质（**不调也可**：默认完全不透明、写深度）。每帧重置为默认。
  ///
  /// `opacity < 1` 时网格进入**透明队列**（先画不透明、再远→近混合）。
  virtual void set_material(const Material& /*material*/) {}
  /// 设当前纹理（**可选**）：设了之后，带 UV 的网格会采样它。
  ///
  /// ## 为什么纹理是“场景的一个状态”而不是“网格的一部分”
  ///
  /// 三维里的纹理是**材质**，而材质在真实用法里经常跨网格共用（一圈产品都贴同一个 logo），
  /// 也会在同帧内切换（不同面贴不同图要多次绘制 + 换纹理）。
  /// 把它挂在网格上会逼着每次换图重建网格缓存——而那正是我们最不想动的热路径。
  ///
  /// 不调用就是**无纹理**：网格走顶点色，行为与以前完全一致。
  ///
  /// `pixels` 是 **RGBA8、行优先、非预乘**；`width/height` 必须为正。
  /// 空 span 或非法尺寸 = 清除纹理（回到顶点色）。
  virtual void set_texture(std::span<const std::uint8_t> /*pixels*/, int /*width*/,
                           int /*height*/) {}
  /// 画一个网格（可带模型变换）。同一个网格重复绘制会复用已上传的缓冲。
  virtual void draw_mesh(const Mesh& mesh, const math::Mat4& model) = 0;
  /// 结束一帧并把结果**合成**到 `target` 的目的矩形（逻辑坐标）。
  ///
  /// **降级链在这里**：`target` 是本进程的 GPU 画布且本场景也是 GPU 腿时，
  /// 三维纹理直接以着色器资源合成（**零回读**）；否则走 `Surface::draw_canvas`
  /// 的普通路径（软件目标 → 逐像素拷贝，GPU 目标 → 一次上传）。
  /// 这使「有显卡用显卡、没显卡降级」不需要任何调用方分支。
  virtual void end_frame(Surface& target, math::Rect destination, float opacity = 1.0f) = 0;

  /// 把迄今为止提交的绘制**真正跑完**（阻塞到 GPU 空闲）。
  ///
  /// 为何接口上需要它：
  /// 1. **计时**。`draw_mesh` 在 GPU 腿上只是把命令塞进缓冲就返回，不等待就量
  ///    只能量到 CPU 侧 API 开销（实测会得出“GPU 光栅 0.03 ms”这种假数据）。
  /// 2. **帧间可重入**。同一份网格缓冲/常量缓冲要在下一帧被覆盖前确保上一帧
  ///    已经用完（驱动层通常也会保护，但从语义上把它说清楚）。
  ///
  /// 软件腿是同步的，所以它是空操作（立即返回）。
  virtual void flush() = 0;

  /// 渲染尺寸（物理像素）。
  [[nodiscard]] virtual auto width() const noexcept -> int = 0;
  [[nodiscard]] virtual auto height() const noexcept -> int = 0;
  /// 本场景的绘制调用计数（用于确认"确实画了东西"而不是静默空帧）。
  [[nodiscard]] virtual auto draw_calls() const noexcept -> std::uint64_t = 0;
  /// 本场景实际跑在哪条腿上：`"gpu"` / `"software"`。
  ///
  /// 为何必须能查：降级是**无声**的——没有这个接口，"这台机器到底用没用上显卡"
  /// 只能靠帧耗时反推，而那种推法在快机器上永远得出"应该用上了"。
  /// 应用把它上报到 metrics，测试据此断言两条腿都被覆盖。
  [[nodiscard]] virtual auto backend() const -> std::string = 0;

  /// 把当前帧的结果交出为**原生纹理**（不读回显存）。
  ///
  /// 存在的理由（实测）：三维光栅只要 0.06 ms，而“取成像素交给软件画布”要
  /// 100 ms（1080p）——不想办法消掉它，“用显卡画三维”就是负优化。
  /// 上层若是同后端的 GPU 画布，就能拿这个句柄直接采样合成（零拷贝）；
  /// 拿不到（默认空）或后端不匹配时，`end_frame` 自动走回读回退路。
  [[nodiscard]] virtual auto texture() const noexcept -> SceneTexture { return {}; }
};

}  // namespace st::raster
