#pragma once

/// @file three_runtime.hpp
/// @brief **用 three.js 写三维场景**：在框架内嵌的 QuickJS 里跑 three.js，再交给本框架渲染。
///
/// ## 为什么需要这一层（这是"让 AI 能真的做三维"的关键判断）
///
/// 本框架原有的三维入口是 `scene3d_script`（JSON 描述）与 `Scene3D`（过程式调用）。
/// 两者都能出图，但**由模型来写**时有同一个硬伤：
///
/// 模型的训练语料里 three.js 极其丰富，而"霜天/Blender 风格 JSON 场景"几乎没有。
/// 于是同样一句"做一个金属球浮在底座上"，写成 three.js 几乎一次就对，
/// 写成框架自己的 JSON 就要反复试——实测差距不在渲染器，在**表达层是否在语料里**。
///
/// 所以这一层不是"再包一个 API"，而是**换掉表达层**：让模型写它最擅长的 three.js，
/// 由本框架负责把它变成像素。
///
/// ## 不引浏览器（这是硬约束）
///
/// three.js 有一半是浏览器相关的（`WebGLRenderer` 要 canvas/WebGL 上下文，
/// `TextureLoader` 要 `Image`/`document`）。本层**只用它的数据侧**：
///
/// | three.js 的部分 | 本层怎么处理 |
/// |---|---|
/// | `Scene` / `Object3D` / `Group` | 用（父子变换、`updateMatrixWorld` 都是纯 JS） |
/// | `BufferGeometry` 及全部内置几何生成器 | 用（顶点/法线/UV/索引直接从 `Float32Array` 读内存） |
/// | `Material`（Standard/Physical/Lambert/Phong/Basic…） | 用参数（color/metalness/roughness/opacity/emissive/side） |
/// | `PerspectiveCamera` / `OrthographicCamera` | 用（位置/朝向/fov/near/far → 本框架 `Camera`） |
/// | `Light`（Directional/Point/Spot/Hemisphere/Ambient） | 用（→ 本框架 `Lighting` / `extra_lights`） |
/// | `WebGLRenderer` | **不用**（渲染由本框架的 `Scene3D` 做） |
/// | `TextureLoader` / `GLTFLoader` / `Image` / `document` | **不用**（贴图走本框架的 `texture` 字段） |
///
/// 一句话：**three.js 只当"场景描述 DSL"，光栅化始终是本框架自己的腿**。
/// 因此它也自然继承了本框架的两条腿（有显卡走 D3D11、没有走软件光栅器）与色彩管线。
///
/// ## 实测数据（构建本层时的探针结论，见 `docs/three3d.md`）
///
/// | 项 | 实测值 |
/// |---|---|
/// | three.js 0.160 在 QuickJS 里编译 + 求值 | **55 ms** |
/// | 常驻内存（加载后） | **4.5 MiB**（默认 16 MiB 上限需要放宽到 64 MiB） |
/// | 每帧推进（旋转 + 求矩阵 + 遍历） | **54 µs** |
/// | 模型高频 API 面（Extrude/Lathe/Tube/Shape+holes/InstancedMesh/AnimationMixer…） | **20/20 通过** |
/// | 端到端（three.js 场景 → 本框架渲染 → PNG 像素判据） | **全绿** |
///
/// ## 线程模型
///
/// 一个 `ThreeRuntime` 绑定一个线程（QuickJS 运行时非线程安全）。内部按线程缓存一个
/// 已加载 three.js 的引擎实例——加载要 55ms，每帧重载是不可接受的。
///
/// ## 用法
///
/// ```cpp
/// auto runtime = st::raster::ThreeRuntime::instance();   // 线程内单例
/// auto scene = runtime->evaluate(R"JS(
///   const scene = new THREE.Scene();
///   const camera = new THREE.PerspectiveCamera(45, 16/9, 0.1, 100);
///   camera.position.set(0, 1.2, 4.5);
///   camera.lookAt(0, 0.3, 0);
///   scene.add(new THREE.AmbientLight(0xffffff, 0.6));
///   const key = new THREE.DirectionalLight(0xffffff, 2.5);
///   key.position.set(3, 5, 4);
///   scene.add(key);
///   scene.add(new THREE.Mesh(
///     new THREE.SphereGeometry(0.7, 32, 24),
///     new THREE.MeshStandardMaterial({ color: 0xc8a07a, metalness: 0.9, roughness: 0.2 })));
///   scene;   // ← 最后一个表达式的值就是场景
/// )JS");
/// ```
/// 返回值是**平台中立的抽取值**（几何 + 材质 + 相机 + 灯光），可直接交给
/// `SceneSpec` / 裁云的 `model_script`，也可自己 `Scene3D::draw_mesh`。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/scene3d.hpp"

namespace st::raster {

/// 从 three.js 场景里抽出来的一个可画物体。
struct ThreeObject {
  /// three.js 里的 `mesh.name`（同名节点会带路径，便于报错指路）。
  std::string name{};
  /// 几何：**已展开成非索引/索引两种都可**的顶点表（`Mesh` 的交错布局）。
  Mesh mesh{};
  /// 材质（PBR 子集）。
  Material material{};
  /// 面的朝向剔除（由 three 的 `material.side` 映射：Front→Back 剔除，Back→Front，Double→不剔）。
  CullMode cull{CullMode::Back};
  /// 世界矩阵（three 的 `matrixWorld`，**列主序**，与本框架 `Mat4` 布局一致，无需转置）。
  math::Mat4 world{math::Mat4::identity()};
  /// three 的 `mesh.visible`（含祖先可见性，已求过）。
  bool visible{true};

  // ── 自转 / 振荡（`userData` 驱动，2026-11-05）──
  //
  // ## 为何不让脚本每帧重求值
  //
  // 真实的三维场景（比如机械表轮系）几何量大，重建一次 `ExtrudeGeometry`
  // 要几十毫秒——而绝大多数“动画”其实只是 **`Object3D` 的旋转/摆动**。
  //
  // 所以这里只抽取一个**意图**：
  //
  // ```js
  // gear.userData.spin        = { axis: [0,0,1], speed: 0.6 };            // 弧度/秒
  // wheel.userData.oscillate  = { axis: [0,0,1], amplitude: 0.9, frequency: 2.5 };
  // ```
  //
  // 视口每帧用它算一个增量旋转，乘在已经烘焙好的世界矩阵后面——
  // **零重求值、零重建**，而观感与“真的在转”完全一致。
  //
  // 这是刻意的分担：几何归脚本（一次），运动归渲染（每帧）。
  math::Vec3 spin_axis{0.0f, 0.0f, 1.0f};
  float spin_speed{0.0f};
  math::Vec3 oscillate_axis{0.0f, 0.0f, 1.0f};
  float oscillate_amplitude{0.0f};
  float oscillate_frequency{0.0f};
  /// 振荡相位（弧度；多组摆轮错相用，默认 0）。
  float oscillate_phase{0.0f};

  /// 本物体是否需要按时间推进（任一通道非零）。
  [[nodiscard]] auto animated() const noexcept -> bool {
    return spin_speed != 0.0f || oscillate_amplitude != 0.0f;
  }

  /// 取景时**不算**本物体（`mesh.userData.noFrame = true`）。
  ///
  /// 用于背景板/地面板这类“氛围件”：它们通常比主体大一圈，
  /// 算进包围盒会让取景距离白白拉远——实测：表径 40 而背景盘半径 22.4，
  /// 把取景距离从 86 拉到 120，主体在画面里只剩一半大。
  bool no_frame{false};
};

/// 抽出来的完整场景（相机 + 灯光 + 物体）。
struct ThreeScene {
  std::vector<ThreeObject> objects{};
  Camera camera{};
  Lighting lighting{};
  /// 抽不到相机时的诊断（例如脚本里没有 camera）。
  std::vector<std::string> warnings{};
  /// 场景里出现的 three.js 版本（由 `THREE.REVISION` 读）。
  std::string three_revision{};

  [[nodiscard]] auto empty() const noexcept -> bool { return objects.empty(); }
};

/// three.js 运行时（线程内单例，内部持有已加载 three.js 的脚本引擎）。
///
/// 为什么不暴露"任意 eval"：本层的契约是**场景进、场景出**。放开任意执行会让
/// 调用方（尤其生成代码的模型）误以为可以在这里做别的事（读文件、连网络），
/// 而那些能力本框架明确不给脚本。
class ThreeRuntime {
 public:
  ThreeRuntime();
  ~ThreeRuntime();
  ThreeRuntime(const ThreeRuntime&) = delete;
  auto operator=(const ThreeRuntime&) -> ThreeRuntime& = delete;

  /// three.js 是否可用（内嵌源码可加载、引擎可建）。
  ///
  /// ## 为什么必须能查而不是"失败即崩"
  ///
  /// 应用（尤其是不需要三维的：编辑器、终端）不该因为这条链坏了就起不来。
  /// 调用方据此决定回退到 JSON 场景或给用户一句明确的说明。
  [[nodiscard]] static auto available() -> bool;

  /// 不可用的**具体原因**（可用时返回空串）。
  ///
  /// 与 `gpu_scene3d_unavailable_reason()` 同一立场：`available()` 只给 bool，
  /// 真出问题时"为什么"全被吞掉，排查只能靠猜。
  [[nodiscard]] static auto unavailable_reason() -> std::string;

  /// 求值一段 three.js 脚本，把场景抽出来。
  ///
  /// ## 脚本的契约
  ///
  /// - 脚本**最后一个表达式的值**是场景根：可以是 `Scene`、`Group`、`Mesh` 或它们的数组；
  /// - 相机可以放在场景里（`scene.add(camera)`）或不放——两种都能取到；
  /// - 脚本里可以定义函数、循环、`for` 生成一百个物体（这是它相对 JSON 的核心优势）；
  /// - **没有** `document` / `window` / `fetch` / 文件系统；`TextureLoader` 构造不报错但加载不了。
  ///
  /// 错误：`Parse`（语法/运行时异常，消息含 three.js 侧的行号）、`Timeout`（超时）、
  /// `Invalid`（返回值不是场景）。
  [[nodiscard]] auto evaluate(std::string_view script, std::string_view filename = "<three>")
      -> Result<ThreeScene>;

  /// 本线程复用的运行时（首次调用会加载 three.js，约 55 ms）。
  ///
  /// 为什么是线程内单例：QuickJS 运行时非线程安全，而导演/导出都是"每线程一条流水线"。
  /// 每帧重建引擎要付 55 ms 加载 + 几 MB 分配，完全不可接受。
  [[nodiscard]] static auto instance() -> ThreeRuntime&;

  /// 脚本执行时限（默认 2 s：生成一百个物体的循环也够，而死循环会被打断）。
  void set_timeout_ms(int milliseconds);
  /// 内存上限（默认 64 MiB）。three.js 自身占约 4.5 MiB，余量留给孩子写的场景。
  void set_memory_limit(std::size_t bytes);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace st::raster
