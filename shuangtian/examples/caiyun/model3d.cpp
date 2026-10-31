/// 裁云 · 三维图层实现（见 `model3d.hpp` 的设计说明）。

#include "model3d.hpp"

#include "st/raster/primitives.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/ui/svg.hpp"

namespace caiyun {

namespace {

constexpr float kPi = 3.14159265358979f;

[[nodiscard]] auto to_radians(float degrees) noexcept -> float {
  return degrees * kPi / 180.0f;
}

/// 球坐标 → 世界坐标（Y 轴向上；方位角 0 = +Z 正前方，向右为正）。
///
/// 为何用「方位角/仰角/距离」而不是直接 eye 坐标：写脚本时人想的是
/// “从右前方看”，而不是“眼在 (2.1, 1.7, 2.6)”。球坐标直接对应该意图，
/// 而且改视角时不会把相机弄进模型内部（半径恒定）。
[[nodiscard]] auto orbit_position(float azimuth_deg, float elevation_deg, float distance) noexcept
    -> st::math::Vec3 {
  const float azimuth = to_radians(azimuth_deg);
  const float elevation = to_radians(std::clamp(elevation_deg, -85.0f, 85.0f));
  const float flat = std::cos(elevation) * distance;
  return st::math::Vec3{flat * std::sin(azimuth), distance * std::sin(elevation),
                        flat * std::cos(azimuth)};
}

/// 主光方向（指向光源）。仰角高 = 顶光，方位角决定从哪侧打。
[[nodiscard]] auto light_vector(float azimuth_deg, float elevation_deg) noexcept
    -> st::math::Vec3 {
  return orbit_position(azimuth_deg, elevation_deg, 1.0f).normalized();
}

}  // namespace

auto evaluate_model3d(const Layer& layer, float local_time, float scene_duration)
    -> Model3dPose {
  Model3dPose pose{};
  const float time = local_time < 0.0f ? 0.0f : local_time;

  // 自转：**恒定角速度**（镜头是匀速的，不做缓动——缓动会让“自转”变成“动画”）。
  // 初始角 `model_yaw` 让同一模型在不同镜头里有不同朝向。
  pose.yaw = to_radians(layer.model_yaw + layer.model_spin * time);
  pose.tilt = to_radians(std::clamp(layer.model_tilt, -89.0f, 89.0f));

  // 上下浮动：正弦。用「整数个周期」不合适（会让不同时长的场景结束姿态不同），
  // 所以直接用 time（同一时刻求值恒定 → 确定性判据成立）。
  if (std::abs(layer.model_bob) > 1e-5f && layer.model_bob_hz > 1e-3f) {
    pose.bob_y = layer.model_bob * std::sin(time * layer.model_bob_hz * 2.0f * kPi);
  }

  // 推拉：整段按进度线性（摄影机的推是匀速的）。`cam_dolly_*` 是**倍率**，
  // 所以 1.0 → 1.08 就是“推 8%”，改 `cam_distance` 时不必重算这一对。
  const float span = std::max(0.001f, scene_duration);
  const float progress = std::clamp(time / span, 0.0f, 1.0f);
  const float dolly = layer.cam_dolly_from + (layer.cam_dolly_to - layer.cam_dolly_from) * progress;
  const float distance = std::max(0.6f, layer.cam_distance * dolly);

  pose.eye = orbit_position(layer.cam_azimuth, layer.cam_elevation, distance);
  pose.target = st::math::Vec3{0.0f, pose.bob_y, 0.0f};
  pose.fov_y_degrees = std::clamp(layer.cam_fov, 8.0f, 110.0f);
  pose.light_direction = light_vector(layer.light_azimuth, layer.light_elevation);
  pose.ambient = std::clamp(layer.light_ambient, 0.0f, 1.0f);
  pose.rim = std::clamp(layer.light_rim, 0.0f, 1.0f);
  return pose;
}

namespace {

/// 网格缓存键：形状串 + 颜色 + 细分。三者任一变化才重建。
///
/// 为何不把 `asset_basis` 放进键：OBJ 路径解析已经把它折进了形状串
/// （`mesh_for_layer` 先 `resolve`），进来时已经是绝对路径。
using MeshKey = std::tuple<std::string, std::uint32_t, int, int, bool>;

struct MeshCache {
  std::map<MeshKey, std::shared_ptr<const st::raster::Mesh>> entries{};

  /// 上限：脚本可以让每帧换一种颜色，但真实片子里的模型是个位数。
  /// 超了清空而不是 LRU——重建一个球的成本（约 40µs）远比维护链表便宜。
  static constexpr std::size_t kLimit = 64;
};

/// **每线程一份**网格缓存。
///
/// 为何是线程局部：导出按多路并行渲染，共享一个 `std::map` 要么加锁
/// （每帧每图层抢一把锁），要么数据竞争。线程局部让缓存天然无竞争，
/// 代价是每个 worker 各存一份（几十 KB）。
[[nodiscard]] auto mesh_cache() -> MeshCache& {
  static thread_local MeshCache cache;
  return cache;
}

[[nodiscard]] auto resolve_obj_path(std::string_view shape, std::string_view asset_basis)
    -> std::string {
  constexpr std::string_view prefix{"obj:"};
  if (shape.size() <= prefix.size()) return {};
  std::string ref{shape.substr(prefix.size())};
  // 与素材库同一套路径语义：绝对路径直接用；否则相对脚本目录
  if (st::fs::is_absolute(ref)) return ref;
  if (!asset_basis.empty()) {
    const std::string joined = st::fs::join(std::string(asset_basis), ref);
    if (st::fs::exists(joined)) return joined;
  }
  return ref;
}

/// 纹理路径解析：与 `resolve_obj_path` 同一套语义（绝对路径 / 相对脚本目录 / 直接给）。
[[nodiscard]] auto resolve_texture_path(std::string_view reference, std::string_view asset_basis)
    -> std::string {
  if (reference.empty()) return {};
  std::string ref{reference};
  if (st::fs::is_absolute(ref)) return ref;
  if (!asset_basis.empty()) {
    const std::string joined = st::fs::join(std::string(asset_basis), ref);
    if (st::fs::exists(joined)) return joined;
  }
  return ref;
}

}  // namespace

auto mesh_for_layer(const Layer& layer, std::string_view asset_basis)
    -> std::shared_ptr<const st::raster::Mesh> {
  const bool is_obj = layer.model_shape.rfind("obj:", 0) == 0;
  const std::string shape_ref =
      is_obj ? resolve_obj_path(layer.model_shape, asset_basis) : layer.model_shape;
  // 颜色折进键：同一几何换色要重建（顶点色存在顶点里），但不必每帧重建。
  const auto color_key = (static_cast<std::uint32_t>(layer.model_color.r) << 24U) |
                         (static_cast<std::uint32_t>(layer.model_color.g) << 16U) |
                         (static_cast<std::uint32_t>(layer.model_color.b) << 8U);
  const auto alt_key = (static_cast<std::uint32_t>(layer.model_color_alt.r) << 24U) |
                       (static_cast<std::uint32_t>(layer.model_color_alt.g) << 16U) |
                       (static_cast<std::uint32_t>(layer.model_color_alt.b) << 8U);
  const MeshKey key{shape_ref, color_key,
                    std::clamp(layer.model_segments, 3, 96), static_cast<int>(alt_key),
                    layer.model_color_alt.a != 0};

  MeshCache& cache = mesh_cache();
  if (const auto found = cache.entries.find(key); found != cache.entries.end()) {
    return found->second;
  }

  using st::math::Color;
  const Color base = layer.model_color;
  auto mesh = std::make_shared<st::raster::Mesh>();
  const int segments = std::clamp(layer.model_segments, 3, 96);

  if (is_obj) {
    const auto loaded = st::raster::Mesh::load_obj_file(shape_ref, base);
    if (!loaded.has_value()) {
      // 模型缺失**不抛**：渲染侧画占位框，让“模型没进来”看得见（而不是静默空白）。
      ST_LOG_WARN("裁云三维：OBJ 加载失败（{}）：{}", shape_ref, loaded.error().message);
      return nullptr;
    }
    *mesh = loaded.value();
  } else if (layer.model_shape == "sphere") {
    *mesh = st::raster::Mesh::colored_sphere(0.85f, segments, base);
  } else if (layer.model_shape == "torus") {
    *mesh = st::raster::Mesh::torus(0.62f, 0.24f, segments, std::max(6, segments / 2), base);
  } else if (layer.model_shape == "cylinder") {
    *mesh = st::raster::Mesh::cylinder(0.5f, 1.15f, segments, base);
  } else if (layer.model_shape == "plane") {
    *mesh = st::raster::Mesh::plane(1.6f, 1.6f, base);
  } else if (layer.model_shape == "textured_box") {
    // 六面都贴同一张图标纹理。
    //
    // ## 为何回到这个做法（踩过，别重复）
    //
    // 曾试图改成“只有正面贴图、侧面纯色”，连续三版都失败：
    // 通过 UV 角带取侧面色时，`kTopInset` 与角带宽度取了同一量级，
    // 双线性采样在边界处把侧面深色抹进了正面，整块图标发暗、图标消失。
    // 调整 `kTopInset`/`kSideGutter` 后又把整张桌面纹理弄暗了。
    //
    // 结论：**六面贴图是能工作的**，代价只是“侧面也带一点图案”——
    // 在本片的透视与尺度下几乎看不出。先保住可用，再谈优化。
    *mesh = st::raster::Mesh::textured_box(1.15f, 1.15f, 1.15f, base);
  } else if (layer.model_shape == "textured_plane") {
    *mesh = st::raster::Mesh::textured_plane(1.6f, 1.6f, base);
  } else if (layer.model_shape == "textured_slab") {
    // 立体扁平块：宽 2.0 / 厚 0.06 / 深 1.5（16:12 的“桌面”比例）。
    // 厚度取 6% ：能看到明显的侧壁成形，但不会变成“一块砖”。
    *mesh = st::raster::Mesh::textured_slab(2.0f, 0.06f, 1.5f, base);
  } else if (const auto kind = st::raster::primitives::parse_kind(layer.model_shape);
             kind.has_value()) {
    // ── 框架图元库（2026-10-30 接入）──
    //
    // 之前这里只有五种内置形状 + OBJ 文件，超出范围的形状只能落到"默认盒子"：
    // 用户写 `capsule` 会得到一只盒子，而且**没有任何提示**。
    // 现在把全部图元（capsule / cone / gear / prism / tube / wedge / 正多面体 / helix…）
    // 接到同一个入口，图元参数从图层字段派生（`model_segments` 是共用的细分档）。
    //
    // ## 为什么颜色走"顶点色"而不是材质
    //
    // 裁云的图层已有 `model_color` / `model_color_alt` 做双色渐变，那套机制读的是
    // **顶点色**。图元生成器接受 `color` 并直接写进顶点，正好接上；
    // 换成材质色会让渐变逻辑失效（渐变是逐顶点插值，材质只有一个基色）。
    st::raster::primitives::PrimitiveParams params{};
    params.radius = 0.6f;
    params.depth = 1.15f;
    params.height = 1.15f;
    params.segments = segments;
    params.rings = std::clamp(segments / 2, 4, 48);
    params.major_segments = segments;
    params.minor_segments = std::clamp(segments / 2, 6, 48);
    params.major_radius = 0.55f;
    params.minor_radius = 0.22f;
    params.subdivisions = 2;
    params.columns = 6;
    params.rows = 6;
    params.color = base;
    *mesh = st::raster::primitives::make(*kind, params);
  } else {
    // 未知形状落到盒子：写错一个形状名不该让整帧没有主体。
    // ⚠ 但**必须**留一条日志——静默兜底等于让"名字拼错"永远查不出来。
    ST_LOG_WARN("裁云三维：未知形状 \"{}\"（可用：{}），已退回盒子", layer.model_shape,
                st::raster::primitives::all_kind_names());
    *mesh = st::raster::Mesh::colored_box(1.15f, 1.15f, 1.15f, base);
  }

  // 双色渐变：第二色非透明时，按顶点的竖直位置在原色与次色之间插值。
  // 这一步在网格生成之后做，是因为它需要知道每个顶点的 y（各生成器的
  // 竖直范围不同），而在生成器里再加一路颜色参数会把那组接口膨胀一倍。
  if (layer.model_color_alt.a != 0) {
    const Color alt = layer.model_color_alt;
    float min_y = 1e9f;
    float max_y = -1e9f;
    for (std::size_t vertex = 0; vertex < mesh->vertex_count(); ++vertex) {
      const float y = (*mesh)[vertex][1];
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
    }
    const float span = max_y - min_y;
    if (span > 1e-5f) {
      for (std::size_t vertex = 0; vertex < mesh->vertex_count(); ++vertex) {
        const std::size_t offset = vertex * st::raster::Mesh::kVertexFloats;
        const float t = ((*mesh)[vertex][1] - min_y) / span;
        const auto mix = [t](std::uint8_t a, std::uint8_t b) -> float {
          return (static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t) /
                 255.0f;
        };
        const float shade = 1.0f + (t - 0.5f) * 0.24f;   // 与生成器的竖直渐变同口径
        mesh->vertices[offset + 6] = std::min(mix(base.r, alt.r) * shade, 1.0f);
        mesh->vertices[offset + 7] = std::min(mix(base.g, alt.g) * shade, 1.0f);
        mesh->vertices[offset + 8] = std::min(mix(base.b, alt.b) * shade, 1.0f);
      }
    }
  }

  if (cache.entries.size() >= MeshCache::kLimit) cache.entries.clear();
  const auto inserted = cache.entries.emplace(key, mesh);
  return inserted.first->second;
}

namespace {

/// 三维场景的复用槽（**每线程一份**，按「宽 + 高 + 目标能否零拷贝」重建）。
///
/// 为何要复用：1080p 场景持有 8MB 颜色目标 + 8MB 深度缓冲。每帧销毁重建 =
/// 每帧两次大分配 + 显存重新提交，在导出 300 帧时非常可观。
/// 裁云是逐帧顺序渲染的，同一尺寸完全可复用；换尺寸才重建。
struct SceneSlot {
  std::unique_ptr<st::raster::Scene3D> scene{};
  int width{0};
  int height{0};
  /// 建这个场景时“合成目标能不能零拷贝接住纹理”的取值（见 `acquire`）。
  bool gpu_target{false};

  /// 取一个尺寸与目标能力都匹配的场景（必要时重建）。失败返回 `nullptr`（不抛）。
  ///
  /// ## `gpu_target` 为何是选腿依据（实测数字）
  ///
  /// 同一份 `sample-3d.json` 在 1080p 导出（目标 = **软件**画布）：
  ///
  /// | 三维腿 | 每帧渲染耗时 |
  /// |---|---|
  /// | GPU（结果回读→软件画布） | **2234 ms** |
  /// | 软件（直接逐像素写画布） | **181 ms** |
  ///
  /// 差 12 倍，是**负优化**。根因不是显卡慢（光栅只要 0.07ms），而是：
  /// ① 目标接不住纹理 → 三维结果要从显存回读（每图层每帧一次）；
  /// ② 回读要同步等 GPU，而导出是 16 路并行 → 16 个线程在上下文锁上排队。
  ///
  /// 所以「有显卡就用显卡」的完整说法是：**显卡只在纹理能留在显存里时才值**。
  /// 目标接不住时主动落软件腿，两边都不吃亏——这正是“不搞负优化”的含义。
  [[nodiscard]] auto acquire(int want_width, int want_height, bool want_gpu_target)
      -> st::raster::Scene3D* {
    if (scene != nullptr && width == want_width && height == want_height &&
        gpu_target == want_gpu_target) {
      return scene.get();
    }
    // ## 目标接不住纹理时必须用软件腿（实测结论，勿改成"让框架自动选"）
    //
    // 这里曾尝试过"交给 Scene3D::create 实测择优"，结果是**灾难**：
    // GPU 腿在回读模式下每帧要把 8MB 颜色缓冲从显存搬回内存 + 同步等 GPU，
    // 实测从 63ms/帧 退化到几分钟都跑不完（16 路并行全堵在上下文锁上）。
    //
    // 框架的 `probe_leg_ms` 测不出这个问题：它量的是"画一帧"，而回读的成本
    // 在 `end_frame` 之后、并且随**并行度**放大——单线程探测看到的是
    // "GPU 0.06ms vs 软件 177ms"，完全是假象。
    //
    // 所以这条判据必须保留在这里：**目标能零拷贝（Surface）才用 GPU**。
    // 等 ① 的零拷贝路径打通后，这个常量才会变成 true。
    const char* previous = std::getenv("ST_SCENE3D_BACKEND");
    const std::string saved = previous != nullptr ? previous : "";
    const bool had = previous != nullptr;
    if (!want_gpu_target) (void)::_putenv_s("ST_SCENE3D_BACKEND", "software");
    auto created = st::raster::Scene3D::create(want_width, want_height);
    if (!want_gpu_target) (void)::_putenv_s("ST_SCENE3D_BACKEND", had ? saved.c_str() : "");
    if (!created.has_value()) {
      ST_LOG_WARN("裁云三维：建场景失败（{}×{}）：{}", want_width, want_height,
                  created.error().message);
      scene.reset();
      width = 0;
      height = 0;
      return nullptr;
    }
    scene = std::move(created.value());
    width = want_width;
    height = want_height;
    gpu_target = want_gpu_target;
    return scene.get();
  }
};

[[nodiscard]] auto scene_slot() -> SceneSlot& {
  static thread_local SceneSlot slot;
  return slot;
}

/// 一张已解码的 RGBA8 纹理（非预乘、行优先）——与 `Scene3D::set_texture` 的契约一致。
struct LoadedTexture {
  std::vector<std::uint8_t> pixels{};
  int width{0};
  int height{0};
  [[nodiscard]] auto ok() const noexcept -> bool {
    return !pixels.empty() && width > 0 && height > 0;
  }
};

/// 纹理边长上限。图标的贴图不需要 4K：1024 已经比它在画面里的像素尺寸大，
/// 再大只是每帧多传显存、且在降采样时引入摩尔纹。
constexpr int kMaxTextureSize = 1024;

/// 程序化生成桌面纹理（定义见文件下方）。
[[nodiscard]] auto make_desktop_texture(int width, int height) -> LoadedTexture;

/// 读一个素材为 RGBA8。支持 SVG（矢量光栅化）与 PNG（解码）。
///
/// 为何要支持 SVG：歌白自家图标就是 SVG，而三维贴图需要位图。
/// 走 SVG 光栅化而不是“先手工转 PNG”是因为：图标改一版就得重导一次图，
/// 那条路必然漂移（手工步骤不会写进仓库）。
[[nodiscard]] auto load_texture_rgba(std::string_view reference, std::string_view asset_basis)
    -> LoadedTexture {
  LoadedTexture out{};
  if (reference.empty()) return out;
  // 特殊引用 `desktop:` —— 程序化生成桌面纹理（不依赖任何图片文件）。
  //
  // 格式：`desktop:` 或 `desktop:宽x高`（缺省 2048x1536，即 4:3 的“屏幕”比例）。
  // 为何用前缀而不是“没有文件就自动生成”：渲染侧的“猜行为”会漂，
  // 用户写错文件名时会静默拿到一个桌面而不是报错。
  constexpr std::string_view kDesktopPrefix{"desktop:"};
  if (reference.rfind(kDesktopPrefix, 0) == 0) {
    int width = 2048;
    int height = 1536;
    std::string spec{reference.substr(kDesktopPrefix.size())};
    if (!spec.empty()) {
      const auto split = spec.find('x');
      if (split != std::string::npos) {
        const int parsed_w = std::atoi(spec.substr(0, split).c_str());
        const int parsed_h = std::atoi(spec.substr(split + 1).c_str());
        if (parsed_w > 0 && parsed_h > 0) {
          width = parsed_w;
          height = parsed_h;
        }
      }
    }
    return make_desktop_texture(std::min(width, kMaxTextureSize),
                                std::min(height, kMaxTextureSize));
  }
  // 路径解析：绝对路径 / 相对脚本目录 / 素材库名（与 image 图层同一套口径）
  const std::string path = resolve_texture_path(reference, asset_basis);
  if (path.empty()) {
    ST_LOG_WARN("裁云三维：纹理找不到（{}）", reference);
    return out;
  }
  std::string lower = path;
  for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const bool is_svg = lower.size() > 4 && lower.substr(lower.size() - 4) == ".svg";
  if (is_svg) {
    // SVG → 光栅：按 512 渲（图标本身是 32×32 设计，512 已是 16 倍超采样）
    constexpr int kSvgSize = 512;
    auto file = st::fs::read_text(path);
    if (!file.has_value()) {
      ST_LOG_WARN("裁云三维：SVG 读失败（{}）", path);
      return out;
    }
    const auto document = st::ui::svg::parse(file.value());
    if (!document.has_value() || document->is_empty()) {
      ST_LOG_WARN("裁云三维：SVG 解析为空（{}）", path);
      return out;
    }
    st::raster::Canvas target{kSvgSize, kSvgSize, 1.0f};
    // 底色：SVG 里没画到的地方（例如外层圆角矩形之外的四个角）是透明的。
    //
    // ⚠ 底色不能是透明：三维纹理与顶点色是**相乘**关系，而透明区的 RGB 是 0，
    // 相乘后那些像素会变成**纯黑**（而且 alpha 在三维渲染里不参与），
    // 于是盒子棱角会出现黑边（实测看到的就是这个）。
    // 用图标自身的主题色铺底，透明区就自然融合成“同色背景”。
    constexpr st::math::Color kIconBackground{0x4D, 0x6B, 0xFE, 255};
    target.clear(kIconBackground);
    st::ui::svg::draw(target, *document,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kSvgSize),
                                     static_cast<float>(kSvgSize)});
    out.width = kSvgSize;
    out.height = kSvgSize;
    out.pixels.resize(static_cast<std::size_t>(kSvgSize) * static_cast<std::size_t>(kSvgSize) *
                      4U);
    const std::span<const std::uint32_t> pixels = target.pixels();
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      const std::uint32_t pixel = pixels[i];
      // 画布是预乘 RGBA（`0xAARRGGBB` 的打包顺序按本仓画布约定）；
      // `set_texture` 要**非预乘**，所以把 R/G/B 除回 alpha。
      const std::uint8_t a = static_cast<std::uint8_t>(pixel & 0xFFU);
      std::uint8_t r = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
      std::uint8_t g = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
      std::uint8_t b = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
      if (a > 0U && a < 255U) {
        const auto unpremultiply = [a](std::uint8_t value) -> std::uint8_t {
          const int scaled = static_cast<int>(value) * 255 / static_cast<int>(a);
          return static_cast<std::uint8_t>(scaled > 255 ? 255 : scaled);
        };
        r = unpremultiply(r);
        g = unpremultiply(g);
        b = unpremultiply(b);
      }
      out.pixels[i * 4U + 0U] = r;
      out.pixels[i * 4U + 1U] = g;
      out.pixels[i * 4U + 2U] = b;
      out.pixels[i * 4U + 3U] = a;
    }
    return out;
  }
  // PNG（其它位图格式暂不支持：三维贴图只需要图标，而图标要么 SVG 要么 PNG）
  auto decoded = st::codec::png_read_file(path);
  if (!decoded.has_value()) {
    ST_LOG_WARN("裁云三维：纹理解码失败（{}）", path);
    return out;
  }
  const st::codec::PngImage& image = decoded.value();
  if (image.width == 0 || image.height == 0) return out;
  // 超限则最近邻缩小（图标不需要高质量重采样；而且这是“防止误用大图”的保护）
  int target_width = static_cast<int>(image.width);
  int target_height = static_cast<int>(image.height);
  const int longest = std::max(target_width, target_height);
  if (longest > kMaxTextureSize) {
    const double ratio = static_cast<double>(kMaxTextureSize) / static_cast<double>(longest);
    target_width = std::max(1, static_cast<int>(static_cast<double>(target_width) * ratio));
    target_height = std::max(1, static_cast<int>(static_cast<double>(target_height) * ratio));
  }
  out.width = target_width;
  out.height = target_height;
  out.pixels.resize(static_cast<std::size_t>(target_width) *
                    static_cast<std::size_t>(target_height) * 4U);
  for (int y = 0; y < target_height; ++y) {
    const std::uint32_t source_y =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(y) * image.height / target_height);
    for (int x = 0; x < target_width; ++x) {
      const std::uint32_t source_x =
          static_cast<std::uint32_t>(static_cast<std::int64_t>(x) * image.width / target_width);
      const std::size_t source_offset =
          (static_cast<std::size_t>(source_y) * static_cast<std::size_t>(image.width) +
           static_cast<std::size_t>(source_x)) *
          4U;
      const std::size_t target_offset =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(target_width) +
           static_cast<std::size_t>(x)) *
          4U;
      for (int channel = 0; channel < 4; ++channel) {
        out.pixels[target_offset + static_cast<std::size_t>(channel)] =
            source_offset + static_cast<std::size_t>(channel) < image.rgba.size()
                ? image.rgba[source_offset + static_cast<std::size_t>(channel)]
                : 255U;
      }
    }
  }
  return out;
}

/// 侧壁色角落的像素宽度（贴图左上角保留这个宽度的纯色带）。
///
/// 为何要 ≥ 8 像素：双线性采样在 UV 靠近边界时会**混合相邻 texel**，
/// 带子太窄时正面的边缘会被侧面深色抹脏（实测：gutter=4 时正面图标发暗）。
/// 现在取 8，配合 `Mesh::kTopInset=0.03`（×512≈15px）留出充分隔离区。
constexpr int kSideGutter = 4;

/// 在贴图**左上角**埋一块侧壁色的纯色带（供 `textured_slab` 的侧壁顶点采样）。
///
/// 做法：把原图缩放到右下区域，左上角与顶部/左侧各一条填侧壁色。
/// **顶面 UV 要相应内缩**（见 `textured_slab` 的 `kTopInset`），
/// 否则图标会被那条色带压进 UV 范围（表现为“画面被推向右下”）。
[[nodiscard]] auto embed_side_gutter(LoadedTexture&& source, st::math::Color side_color)
    -> LoadedTexture {
  if (!source.ok()) return std::move(source);
  const int gutter = kSideGutter;
  const int width = source.width;
  const int height = source.height;
  if (width <= gutter * 2 || height <= gutter * 2) return std::move(source);
  const int inner_width = width - gutter;
  const int inner_height = height - gutter;
  LoadedTexture out{};
  out.width = width;
  out.height = height;
  out.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U,
                    255U);
  // 先整张填侧壁色（含左上角与两条带）
  for (std::size_t i = 0; i < out.pixels.size(); i += 4U) {
    out.pixels[i + 0U] = side_color.r;
    out.pixels[i + 1U] = side_color.g;
    out.pixels[i + 2U] = side_color.b;
    out.pixels[i + 3U] = 255U;
  }
  // 原图缩放到右下区域（最近邻：这里是“把桌面缩一点”，不是最终质量路径）
  for (int y = 0; y < inner_height; ++y) {
    const int source_y = static_cast<int>(static_cast<std::int64_t>(y) * source.height /
                                          inner_height);
    for (int x = 0; x < inner_width; ++x) {
      const int source_x = static_cast<int>(static_cast<std::int64_t>(x) * source.width /
                                            inner_width);
      const std::size_t source_offset =
          (static_cast<std::size_t>(source_y) * static_cast<std::size_t>(source.width) +
           static_cast<std::size_t>(source_x)) *
          4U;
      const std::size_t target_offset =
          (static_cast<std::size_t>(y + gutter) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x + gutter)) *
          4U;
      for (int channel = 0; channel < 4; ++channel) {
        out.pixels[target_offset + static_cast<std::size_t>(channel)] =
            source.pixels[source_offset + static_cast<std::size_t>(channel)];
      }
    }
  }
  return out;
}

/// 程序化生成“桌面”纹理：壁纸 + 图标网格 + 任务栏。
///
/// ## 为何要生成而不是截图
///
/// 截图是**硬编码某一台机器的状态**：壁纸配色、图标、缩放系数、任务栏样式全都钉死，
/// 而且改一个图标就要重新截。生成则把“桌面”变成**参数**（配色、图标数、图标色板），
/// 同一份代码可以出任意风格的桌面，也能随片子的配色调整。
///
/// 代价：画不出真实应用图标（我们没有那些图标的矢量）。所以每个图标是
/// “圆角块 + 几何图形”（圆/三角/条/环），靠**色板差异**让它们可区分。
/// 这在“桌面图标是小色块”的远景里完全够用。
[[nodiscard]] auto make_desktop_texture(int width, int height) -> LoadedTexture {
  LoadedTexture out{};
  if (width <= 0 || height <= 0) return out;
  st::raster::Canvas canvas{width, height, 1.0f};
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  // ① 壁纸：斜向渐变（左上偏蓝紫、右下偏黑），比纯色有空间感
  canvas.clear(st::math::Color{10, 13, 24, 255});
  const int bands = 48;
  for (int i = 0; i < bands; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(bands);
    const auto mix = [t](int from, int to) {
      return static_cast<std::uint8_t>(
          static_cast<double>(from) +
          (static_cast<double>(to) - static_cast<double>(from)) * static_cast<double>(t));
    };
    canvas.fill_rect(st::math::Rect{0.0f, h * t, w, h / static_cast<float>(bands) + 1.0f},
                     st::raster::Paint::solid(st::math::Color{mix(28, 8), mix(34, 10), mix(66, 20), 255}));
  }
  // 背景光晕：右上角一团柔光，让“光照方向”与三维场景一致（光从右上来）
  for (int i = 12; i > 0; --i) {
    const float radius = w * 0.46f * static_cast<float>(i) / 12.0f;
    const auto alpha = static_cast<std::uint8_t>(4 + 3 * (12 - i));
    canvas.fill_circle(st::math::Point{w * 0.78f, h * 0.22f}, radius,
                       st::raster::Paint::solid(st::math::Color{96, 128, 255, alpha}));
  }

  // ② 桌面图标：4 列 × 3 行（靠左排列，与 Windows 默认一致）
  //
  // ## 配色为何是“品牌蓝族的明度阶梯”而不是彩虹
  //
  // 实测（2026-10-22）：第一版用了 12 色彩虹（蓝青绿黄橙红紫粉…），用户判「粗糙」——
  // 量化后是 **10 个独立色族（30°一档）**。而专业产品视觉通常只有 **1~2 个色族**
  // 加一个强调色：多色相在画面里互相争夺注意力，观感就是“廉价”。
  //
  // 所以改成：主色相统一在品牌蓝（`#4D6BFE`，约 224°），靠**明度与饱和度**拉开层次；
  // 只留**一个**暖色图标做视觉焦点（呼应“注视点”原则）。
  const st::math::Color palette[12] = {
      {77, 107, 254, 255},   {92, 128, 255, 255},   {64, 88, 224, 255},
      {110, 146, 255, 255},  {58, 78, 190, 255},    {132, 162, 255, 255},
      {74, 104, 240, 255},   {96, 120, 232, 255},   {70, 94, 210, 255},
      {120, 150, 252, 255},  {240, 178, 80, 255},   {56, 74, 180, 255},
  };
  const int columns = 4;
  const int rows = 3;
  const float cell_w = w * 0.135f;
  const float cell_h = h * 0.195f;
  const float origin_x = w * 0.055f;
  const float origin_y = h * 0.10f;
  const float icon_size = std::min(cell_w, cell_h) * 0.56f;
  int palette_index = 0;
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < columns; ++column) {
      const float cx = origin_x + static_cast<float>(column) * cell_w;
      const float cy = origin_y + static_cast<float>(row) * cell_h;
      const st::math::Color color = palette[palette_index % 12];
      ++palette_index;
      const st::math::Rect tile{cx, cy, icon_size, icon_size};
      // 图标底板（圆角块）
      canvas.fill_rect(tile, st::raster::Paint::solid(color), icon_size * 0.24f);
      // 中央的几何图形：靠形状 + 半透明白区分（营造“图标里有内容”的观感）
      const st::math::Point center{cx + icon_size * 0.5f, cy + icon_size * 0.5f};
      const float glyph = icon_size * 0.24f;
      const st::math::Color white{255, 255, 255, 235};
      switch (palette_index % 4) {
        case 0:
          canvas.fill_circle(center, glyph, st::raster::Paint::solid(white));
          break;
        case 1:
          canvas.fill_rect(st::math::Rect{center.x - glyph, center.y - glyph * 0.35f,
                                          glyph * 2.0f, glyph * 0.7f},
                           st::raster::Paint::solid(white), glyph * 0.3f);
          break;
        case 2:
          canvas.fill_rect(st::math::Rect{center.x - glyph, center.y - glyph * 0.8f,
                                          glyph * 0.55f, glyph * 1.6f},
                           st::raster::Paint::solid(white), glyph * 0.2f);
          canvas.fill_rect(st::math::Rect{center.x + glyph * 0.1f, center.y - glyph * 0.3f,
                                          glyph * 0.55f, glyph * 1.1f},
                           st::raster::Paint::solid(white), glyph * 0.2f);
          break;
        default: {
          // 环：画一个稍大的白色圆、中间叠一个主色圆（无需描边 API 也能出“环形”观感）
          canvas.fill_circle(center, glyph, st::raster::Paint::solid(white));
          canvas.fill_circle(center, glyph * 0.52f, st::raster::Paint::solid(color));
          break;
        }
      }
      // 图标名（小方块占位条：我们没有 TextPort，用一条淡色横条暗示文字）
      canvas.fill_rect(st::math::Rect{cx + icon_size * 0.12f, cy + icon_size + icon_size * 0.10f,
                                      icon_size * 0.76f, icon_size * 0.075f},
                       st::raster::Paint::solid(st::math::Color{210, 224, 255, 150}),
                       icon_size * 0.03f);
    }
  }

  // ③ 任务栏：贴底部，半透明黑 + 居中开始按钮 + 几个小图标
  const float bar_height = h * 0.085f;
  const float bar_top = h - bar_height;
  canvas.fill_rect(st::math::Rect{0.0f, bar_top, w, bar_height},
                   st::raster::Paint::solid(st::math::Color{16, 19, 30, 232}));
  // 顶边一条细分隔线（Windows 11 的观感）
  canvas.fill_rect(st::math::Rect{0.0f, bar_top, w, 1.5f},
                   st::raster::Paint::solid(st::math::Color{70, 86, 140, 170}));
  // 居中的任务栏图标（五个）+ 开始按钮（四个小方块拼成的视窗标）
  const float dock_icon = bar_height * 0.46f;
  const float gap = dock_icon * 0.55f;
  const float total = 5.0f * dock_icon + 4.0f * gap;
  float dock_x = w * 0.5f - total * 0.5f;
  const float dock_y = bar_top + (bar_height - dock_icon) * 0.5f;
  for (int i = 0; i < 5; ++i) {
    const st::math::Color color = palette[(i * 3 + 1) % 12];
    const st::math::Rect tile{dock_x, dock_y, dock_icon, dock_icon};
    canvas.fill_rect(tile, st::raster::Paint::solid(color), dock_icon * 0.26f);
    // 运行中的下划线
    canvas.fill_rect(st::math::Rect{dock_x + dock_icon * 0.28f, bar_top + bar_height * 0.90f,
                                    dock_icon * 0.44f, bar_height * 0.045f},
                     st::raster::Paint::solid(st::math::Color{140, 170, 255, 230}),
                     bar_height * 0.02f);
    dock_x += dock_icon + gap;
  }

  // ④ Canvas（预乘 RGBA）→ 非预乘 RGBA8（`set_texture` 的契约）
  out.width = width;
  out.height = height;
  out.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  const std::span<const std::uint32_t> pixels = canvas.pixels();
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const std::uint32_t pixel = pixels[i];
    const std::uint8_t a = static_cast<std::uint8_t>(pixel & 0xFFU);
    const auto unpremultiply = [a](std::uint8_t value) -> std::uint8_t {
      if (a == 0U) return 0U;
      if (a == 255U) return value;
      const int scaled = static_cast<int>(value) * 255 / static_cast<int>(a);
      return static_cast<std::uint8_t>(scaled > 255 ? 255 : scaled);
    };
    out.pixels[i * 4U + 0U] = unpremultiply(static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU));
    out.pixels[i * 4U + 1U] = unpremultiply(static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU));
    out.pixels[i * 4U + 2U] = unpremultiply(static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU));
    out.pixels[i * 4U + 3U] = a;
  }
  return out;
}

/// 给场景上传本图层的纹理（带内容缓存：同一张图每帧不重复解码）。
///
/// 为何必须缓存：SVG 光栅化 512×512 要几毫秒，而导出是逐帧调用。
/// 不缓存的话 300 帧片会重复光栅化 300 次——那是纯粹的浪费。
void upload_layer_texture(st::raster::Scene3D& scene, const Layer& layer,
                          std::string_view asset_basis) {
  if (layer.model_texture.empty()) {
    scene.set_texture({}, 0, 0);
    return;
  }
  struct CacheEntry {
    std::string key;
    LoadedTexture texture;
  };
  static thread_local std::vector<CacheEntry> cache;
  const std::string key = std::string(asset_basis) + "\n" + layer.model_texture;
  for (const CacheEntry& entry : cache) {
    if (entry.key == key) {
      scene.set_texture(entry.texture.pixels, entry.texture.width, entry.texture.height);
      return;
    }
  }
  LoadedTexture loaded = load_texture_rgba(layer.model_texture, asset_basis);
  // 平板需要“侧壁色角落”：把纹理扩到 `kSideGutter` 像素，并把那块填成侧壁色。
  //
  // 为何用“扩边”而不是单独一张 1×1 纹理：网格是**单个** draw 调用（一次绑一张纹理），
  // 顶面与侧壁没法用两张图。于是把侧壁色伴在贴图角落里，侧壁顶点的 UV
  // 钉在那个角上（`Mesh::kSideU/kSideV`），双线性采样就只采到纯色。
  //
  // 为何用 0.002 这种“很靠近原点”的坐标：`kSideGutter` 像素宽的角落
  // 在归一化坐标下就是 0..gutter/width，只要坐标落在它内部，四个采样点全在纯色区。
  if ((layer.model_shape == "textured_slab" || layer.model_shape == "textured_box") &&
      loaded.ok()) {
    const st::math::Color gutter_color =
        layer.model_color_alt.a != 0 ? layer.model_color_alt : layer.model_color;
    loaded = embed_side_gutter(std::move(loaded), gutter_color);
  }
  scene.set_texture(loaded.pixels, loaded.width, loaded.height);
  // 缓存上限：脚本可能每帧换纹理，但真实剧本用不到 8 张以上
  if (cache.size() >= 8U) cache.erase(cache.begin());
  cache.push_back(CacheEntry{key, std::move(loaded)});
}

}  // namespace

void paint_model3d(st::raster::Surface& canvas, const Layer& layer, st::math::Rect box,
                   float local_time, float scene_duration, float alpha,
                   std::string_view asset_basis, bool target_is_gpu) {
  if (alpha <= 0.004f || box.width < 2.0f || box.height < 2.0f) return;


  // ## 场景脚本路径（优先于单网格）
  //
  // 它自带相机/光照/多物体，与图层的 `model_*` 语义互斥（见 `Layer::model_script`）。
  //
  // ## 为何用 `render_scene_into` 而不是 `render_scene`（实测死锁）
  //
  // `render_scene` 内部每次 `Scene3D::create`，而 `create` 会读写环境变量
  // `ST_SCENE3D_BACKEND`；本函数下方的 `slot.acquire` 为了让结果能回读到
  // 软件画布，也在这个变量上做了「临时设 software、用完还原」。
  // 两者叠加会**破坏宿主的保存/恢复**——实测：导出卡在 33/180 帧，
  // 控制通道完全超时（进程还在，什么都不响应）。
  //
  // 所以这里复用槽位里已有的实例，不新建。

  const int width = std::max(2, static_cast<int>(std::lround(box.width)));
  const int height = std::max(2, static_cast<int>(std::lround(box.height)));
  // “目标能不能零拷贝接住纹理”：只有 GPU 画布能。
  //
  // 为何要在这里判：`Canvas&` 的参数类型只能是软件画布（GPU 画布是
  // `gpu::create_canvas` 返回的 `Surface`，不继承 `Canvas`）——所以**当前调用面
  // 永远是软件目标**。意思是：三维结果必回读，GPU 腿在这条路上是负优化。
  // 这里显式传 `false` 把决策写清楚，而不是让 `Scene3D::create` 在进程层面
  // 隐式选 GPU 然后每帧回读。
  //
  // 将来若把渲染管线改成 `Surface&`（P1 计划），这里改成查目标能力即可，
  // 三维就能拿到 0.07ms 的零拷贝路径。
  // ## 合成目标能不能零拷贝接住 GPU 纹理（2026-10-31）
  //
  // 这是三维能不能用显卡的**唯一开关**。开关为 false 时三维走软件腿
  // （实测 1080p 三物体 344ms/帧）；为 true 时走 GPU 零拷贝
  // （同场景 0.85ms/帧 @800x450，实测快 17.6 倍，见 `tests/gpu_zero_copy_test.cpp`）。
  //
  // **前提**：合成画布必须是 `GpuCanvas`（`CAIYUN_GPU=1` 时才是）。
  // 软件画布没有 `native_texture()`，接不住纹理——那时若仍走 GPU 腿，
  // `end_frame` 会自己回退成"回读"，反而比软件腿慢十几倍。
  //
  // 所以这里查**运行时**能力，不再写常量：目标是不是 GPU 画布由 `dispatch`
  // 传入（见 `paint_model3d` 的 `target_is_gpu` 参数）。
  const bool kTargetAcceptsTexture = target_is_gpu;
  SceneSlot& slot = scene_slot();
  st::raster::Scene3D* scene = slot.acquire(width, height, kTargetAcceptsTexture);
  if (scene == nullptr) {
    // 场景建不起来（极端情况：内存/驱动）：画一块醒目占位而不是静默空白，
    // 否则“三维图层没东西”会被当成设计。
    canvas.fill_rect(box, st::raster::Paint::solid(st::math::Color{48, 20, 26, 255}));
    return;
  }

  // 场景脚本分支：槽位实例已就绪，直接复用。
  if (!layer.model_script.empty()) {
    const auto spec = st::raster::parse_scene_spec_text(layer.model_script);
    if (!spec) {
      canvas.fill_rect(box, st::raster::Paint::solid(st::math::Color{48, 20, 26, 255}));
      ST_LOG_WARN("裁云三维：model_script 解析失败：{}", spec.error().message);
      return;
    }
    // 场景里的 `obj:` 形状按**脚本所在目录**解析（与贴图 `media/xxx.png`
    // 同一套基准）。不传会让相对路径落到进程工作目录上——预览时碰巧对，
    // 换个目录导出就变成「模型不见了」。
    st::raster::SceneSpec resolved = *spec;
    resolved.asset_basis = std::string(asset_basis);
    st::raster::Canvas offscreen{width, height, 1.0f};
    // ## 必须用 `render_scene_at_into`（带时间），而不是 `render_scene_into`
    //
    // 实测：用静态版本时 `model_script` 里的 `animation` **完全被忽略**——
    // 画面 4 秒不动，而渲染"成功"、无任何报错。这类"功能写了但没接上"
    // 的缺陷只有对比前后帧才能发现（实测画面静止率 88%）。
    //
    // `local_time` 是本图层在所在镜头内的时间（秒），正是动画要的时钟。
    const auto status = st::raster::render_scene_at_into(
        resolved, local_time, *scene, offscreen, st::math::Color{0, 0, 0, 0});
    if (!status) {
      canvas.fill_rect(box, st::raster::Paint::solid(st::math::Color{48, 20, 26, 255}));
      ST_LOG_WARN("裁云三维：model_script 渲染失败：{}", status.error().message);
      return;
    }
    st::raster::DrawOptions options{};
    options.opacity = std::clamp(alpha, 0.0f, 1.0f);
    canvas.draw_canvas(offscreen, box, options);
    return;
  }

  const std::shared_ptr<const st::raster::Mesh> mesh = mesh_for_layer(layer, asset_basis);
  if (mesh == nullptr || mesh->is_empty()) {
    canvas.fill_rect(box, st::raster::Paint::solid(st::math::Color{48, 20, 26, 255}));
    return;
  }
  // 贴图：只对带 UV 的网格生效（无 UV 时 `set_texture` 也不报错，直接就退加顶点色）。
  upload_layer_texture(*scene, layer, asset_basis);

  const Model3dPose pose = evaluate_model3d(layer, local_time, scene_duration);

  // 背景：不透明（用场景底色兜住）或透明（叠在视频背景上）。
  const st::math::Color clear_color =
      layer.model_backdrop ? st::math::Color{layer.model_color.r, layer.model_color.g,
                                            layer.model_color.b, 255}
                           : st::math::Color{0, 0, 0, 0};

  st::raster::Camera camera;
  camera.eye = pose.eye;
  camera.target = pose.target;
  camera.up = st::math::Vec3{0.0f, 1.0f, 0.0f};
  camera.fov_y_degrees = pose.fov_y_degrees;
  camera.near_z = 0.05f;
  camera.far_z = 100.0f;

  st::raster::Lighting lighting{};
  lighting.light_from = pose.light_direction;
  lighting.ambient = pose.ambient;
  lighting.rim = pose.rim;

  // 模型变换：先按矩形长宽比适配，再缩放/倾斜/自转/浮动。
  //
  // 为何要按**短边**归一：图层矩形的长宽比可能不是 1:1（16:9 的画面里
  // 一块竖长卡片），若按长边铺，模型会在短边方向被压扁。按短边算基准尺寸、
  // 长短边都按它取，才能保证“模型永远是圆的”。
  const float short_side = std::min(static_cast<float>(width), static_cast<float>(height));
  // 相机在距离 d、视野 fov 下能看到的竖直高度 = 2·d·tan(fov/2)。
  // 让模型（单位尺寸 ≈ 2）占据其中约 62% 的画面，留出安全边距。
  const float visible_height = 2.0f * pose.eye.length() *
                               std::tan(to_radians(pose.fov_y_degrees) * 0.5f);
  const float fit = visible_height * 0.62f * std::clamp(layer.model_scale, 0.05f, 8.0f);
  const float aspect_box = static_cast<float>(width) / static_cast<float>(height);
  (void)short_side;

  const st::math::Mat4 model =
      st::math::Mat4::translation(st::math::Vec3{0.0f, pose.bob_y, 0.0f}) *
      st::math::Mat4::rotation(st::math::Vec3{0.0f, 1.0f, 0.0f}, pose.yaw) *
      st::math::Mat4::rotation(st::math::Vec3{1.0f, 0.0f, 0.0f}, pose.tilt) *
      st::math::Mat4::scaling(st::math::Vec3{fit * 0.5f, fit * 0.5f, fit * 0.5f});
  (void)aspect_box;

  scene->begin_frame(clear_color);
  scene->set_camera(camera);
  scene->set_lighting(lighting);
  scene->draw_mesh(*mesh, model);

  // 合成进图层矩形。
  //
  // 这里**直接调 `end_frame`**：它会先尝试零拷贝（目标是 GPU 画布时纹理
  // 直接采样，实测 0.07ms vs 回读 37ms），失败才回读。调用方不必判断
  // “我用的是哪条腿”——降级链只在框架里实现一次。
  st::raster::DrawOptions options{};
  options.opacity = std::clamp(alpha, 0.0f, 1.0f);
  scene->end_frame(canvas, box, options.opacity);
}

void paint_model3d_group(st::raster::Surface& canvas, const Layer& head,
                         const std::vector<const Layer*>& members, float local_time,
                         float scene_duration, std::string_view asset_basis,
                         const LayerState& head_state, float canvas_w, float canvas_h,
                         float design_w, float design_h,
                         const std::vector<LayerState>* member_states,
                         bool target_is_gpu) {
  if (head_state.alpha <= 0.004f || head_state.w < 2.0f || head_state.h < 2.0f) return;
  const st::math::Rect box{head_state.x, head_state.y + head_state.offset_y, head_state.w,
                           head_state.h};
  const int width = std::max(2, static_cast<int>(std::lround(box.width)));
  const int height = std::max(2, static_cast<int>(std::lround(box.height)));
  SceneSlot& slot = scene_slot();
  // ## 合成目标能不能零拷贝接住 GPU 纹理（2026-10-31）
  //
  // 这是三维能不能用显卡的**唯一开关**。开关为 false 时三维走软件腿
  // （实测 1080p 三物体 344ms/帧）；为 true 时走 GPU 零拷贝
  // （同场景 0.85ms/帧 @800x450，实测快 17.6 倍，见 `tests/gpu_zero_copy_test.cpp`）。
  //
  // **前提**：合成画布必须是 `GpuCanvas`（`CAIYUN_GPU=1` 时才是）。
  // 软件画布没有 `native_texture()`，接不住纹理——那时若仍走 GPU 腿，
  // `end_frame` 会自己回退成"回读"，反而比软件腿慢十几倍。
  //
  // 所以这里查**运行时**能力，不再写常量：目标是不是 GPU 画布由 `dispatch`
  // 传入（见 `paint_model3d` 的 `target_is_gpu` 参数）。
  const bool kTargetAcceptsTexture = target_is_gpu;
  st::raster::Scene3D* scene = slot.acquire(width, height, kTargetAcceptsTexture);
  if (scene == nullptr) {
    canvas.fill_rect(box, st::raster::Paint::solid(st::math::Color{48, 20, 26, 255}));
    return;
  }

  // 相机/光照/推拉都取 head 的（一个世界只有一台相机、一盏主光）。
  const Model3dPose pose = evaluate_model3d(head, local_time, scene_duration);
  st::raster::Camera camera;
  camera.eye = pose.eye;
  camera.target = pose.target;
  camera.up = st::math::Vec3{0.0f, 1.0f, 0.0f};
  camera.fov_y_degrees = pose.fov_y_degrees;
  camera.near_z = 0.05f;
  camera.far_z = 100.0f;
  st::raster::Lighting lighting{};
  lighting.light_from = pose.light_direction;
  lighting.ambient = pose.ambient;
  lighting.rim = pose.rim;

  const st::math::Color clear_color =
      head.model_backdrop
          ? st::math::Color{head.model_color.r, head.model_color.g, head.model_color.b, 255}
          : st::math::Color{0, 0, 0, 0};
  scene->begin_frame(clear_color);
  scene->set_camera(camera);
  scene->set_lighting(lighting);

  // 世界尺寸基准：head 的网格尺寸（与世界 1 单位对齐）。
  const float visible_height =
      2.0f * pose.eye.length() * std::tan(to_radians(pose.fov_y_degrees) * 0.5f);

  // 画面框 → 世界坐标的映射比例。
  //
  // ## 为何是 `visible_height / head.h` 而不是 `0.62*visible_height / head.w`
  //
  // 早先写成后者，结果是：整个画面框只映射到 ±0.31 世界单位，而桌面块本身宽 2.0——
  // 于是所有图标被挤到世界中心附近的一小块里（实测 5 个图标叠在右下角一处）。
  //
  // 正确的关系是：head 的矩形就是**整个可见画面**，所以它的高度对应 `visible_height`、
  // 宽度按纵横比得出。这样“图标在画面里放在哪里”就等价于“它在桌面上哪里”。
  const float world_per_design = visible_height / std::max(1.0f, head_state.h);
  const float center_design_x = head_state.x + head_state.w * 0.5f;
  const float center_design_y = head_state.y + head_state.h * 0.5f;

  /// 一个待绘制的成员（位置先算好，再分两道画）。
struct Placement {
  const Layer* layer{nullptr};
  /// 该成员这一帧的**最终**状态（含 `layout` 增量）；影子要读同一份。
  std::shared_ptr<const LayerState> state{};
  std::shared_ptr<const st::raster::Mesh> mesh{};
    float offset_x{0.0f};
    float offset_z{0.0f};
    float jump{0.0f};
    float scale{1.0f};
  };
  std::vector<Placement> placements;
  placements.reserve(members.size());

  for (std::size_t member_index = 0; member_index < members.size(); ++member_index) {
    const Layer* member = members[member_index];
    if (member == nullptr) continue;
    // 优先用调用方交来的（含 `layout` 增量的）状态；没交才自己求值。
    // 自己求值会丢掉编排层的位移——那是三维成员“能动”的唯一来源。
    const LayerState state =
        (member_states != nullptr && member_index < member_states->size())
            ? (*member_states)[member_index]
            : evaluate_layer(*member, local_time, canvas_w, canvas_h, design_w, design_h);
    if (state.alpha <= 0.004f) continue;
    const std::shared_ptr<const st::raster::Mesh> mesh = mesh_for_layer(*member, asset_basis);
    if (mesh == nullptr || mesh->is_empty()) continue;

    // 成员位移：相对画面框中心的偏移（设计像素）→ 世界单位。
    // X 与 Z 折算到世界时用**同一个比例**（等比例不拉伸）。
    const float dx_design = (state.x + state.w * 0.5f) - center_design_x;
    const float dy_design = (state.y + state.h * 0.5f) - center_design_y;
    // 环境变量翻转 Z（用于实验定位符号错误；正式路径不用）
    const float z_sign = std::getenv("CAIYUN_FLIP_Z") != nullptr ? -1.0f : 1.0f;
    placements.push_back(Placement{member,
                                   std::make_shared<const LayerState>(state),
                                   mesh,
                                   dx_design * world_per_design,
                                   dy_design * world_per_design * z_sign,
                                   -bounce_offset(*member, local_time) * world_per_design,
                                   std::clamp(member->model_scale, 0.01f, 8.0f)});
  }

  // —— 第一道：地面（head）+ 所有物体 ——
  //
  // ## 为何必须分两道画（踩过，必看）
  //
  // 第一版把影子与物体**放在同一个循环里、影子先画**。结果影子虽然提交了，
  // 但物体自身（包括它的底面）先于地面写入 z-buffer 之外后又被地面覆盖/反被遮挡：
  // 实测量出来“物体正下方”与“旁边空地”亮度**完全一样**（都是 0.072）——影子看不见。
  //
  // 正确顺序是：**先画完整场景（含地面），再补影子**。但影子与地面共面会有
  // z-fighting，所以影子必须抬一个微小高度（`flattened` 的 `offset_y`），
  // 且影子的 alpha 必须由渲染侧的混合处理（本渲染器三维混合是不透明的，
  // 所以影子用**与地面混合后的预混色**而不是半透黑——见下方说明）。
  for (const Placement& p : placements) {
    upload_layer_texture(*scene, *p.layer, asset_basis);
    // ── Squash & Stretch（体积守恒，Disney 12 原则之首）──
    //
    // 相位取自弹跳：1 - 归一高度。落地时压缩、顶点时拉伸。
    // 横向按体积守恒补偿：`xz = 1/sqrt(y)`（三维里 X=Z 时 X·X·Y=1）。
    // 用 `1/sqrt` 而不是 `1/y`：后者在压扁时会把横向鼓得夸张。
    float deform_y = 1.0f;
    float deform_xz = 1.0f;
    if (p.layer->model_bounce_squash > 0.001f) {
      const float lift = std::max(0.0f, p.jump);
      // 用“距离地面”的相对量当作相位：刚好落地时 lift≈0
      const float unit = std::max(1e-3f, p.scale);
      const float lift_ratio = std::clamp(lift / (unit * 1.4f), 0.0f, 1.0f);
      const float amount = p.layer->model_bounce_squash;
      // 落地（lift_ratio=0）：Y 压；顶点（lift_ratio=1）：Y 微拉
      const float target_y = 1.0f - 0.20f * (1.0f - lift_ratio) + 0.06f * lift_ratio;
      deform_y = 1.0f + (target_y - 1.0f) * amount;
      deform_xz = 1.0f / std::sqrt(std::max(0.45f, deform_y));
    }

    st::math::Mat4 model = st::math::Mat4::identity();
    model = st::math::Mat4::translation(st::math::Vec3{p.offset_x, p.jump, p.offset_z});
    const float yaw =
        to_radians(p.layer->model_yaw + p.layer->model_spin * local_time);
    const float tilt = to_radians(p.layer->model_tilt);
    model = model * st::math::Mat4::rotation(st::math::Vec3{0.0f, 1.0f, 0.0f}, yaw) *
            st::math::Mat4::rotation(st::math::Vec3{1.0f, 0.0f, 0.0f}, tilt);
    // 形变缩放：Y 与 XZ 分开（就是 squash & stretch 的全部）
    model = model * st::math::Mat4::scaling(
                        st::math::Vec3{p.scale * deform_xz, p.scale * deform_y,
                                       p.scale * deform_xz});
    scene->draw_mesh(*p.mesh, model);
  }

  st::raster::DrawOptions options{};
  options.opacity = std::clamp(head_state.alpha, 0.0f, 1.0f);

  // ── 先把三维结果合成进画布 ──
  //
  // ⚠ 顺序很关键：接触阴影必须在**图标之后**画（能盖在图标底部一点点，
  // 形成“贴地”的过渡），但必须在**桌面之后**（否则会被桌面盖住）。
  // 三维场景内部无法做到这个顺序（它只有深度缓冲、没有图层概念），
  // 所以 3D 先整体合成、随后在画布上用二维椭圆补阴影。
  scene->end_frame(canvas, box, options.opacity);

  // ── 接触阴影：在**二维层**画柔和椭圆 ──
  //
  // ## 为何从三维路径改成二维（这轮最贵的一课）
  //
  // 先在三维场景里用“压平的网格”画影子，花了大量轮次仍不正确：
  // · 压平实体盒会把 6 个面塔到同一高度 → 共面多边形互相争夺，画了看不见；
  // · 改用“只取朝上面”后能看见，但位置只对了一半；
  // · 抬高量会被后续 `scaling` 乘小，影子埋在桌面块内部；
  // · Z 方向符号不确定（两种假设各得到不同结果，而且**两者都不对**）。
  //
  // 根因是**用错工具**：接触阴影需要的不是“光学正确的投影”，而是
  // “物体底部一片比地面暗的柔边”——那是**二维绘制问题**。
  // 在二维层画：位置直接就是图标的屏幕矩形（不会错）、模糊用多层衰减
  // （真正柔和）、且完全不依赖三维深度缓冲与混合模式。
  //
  // 代价：影子不随相机做透视变形（但本片的相机是固定的俯视，影子上
  // 本来就是椭圆，看不出差别）。
  for (const Placement& p : placements) {
    if (p.layer->model_shadow <= 0.001f) continue;
    // 影子必须读**与物体同一份**状态（含 `layout` 增量），否则物体动了、
    // 影留在原地——那是“两个不相干的物体”的观感。
    const LayerState state =
        (p.state != nullptr) ? *p.state
                             : evaluate_layer(*p.layer, local_time, canvas_w, canvas_h,
                                              design_w, design_h);
    if (state.alpha <= 0.004f) continue;
    // 图标在**画面**上的矩形（与它的 state 一致）；影子贴在它底部
    //
    // 注：这里用图层矩形的中心，而三维体经过透视投影后的屏幕中心会因世界 Z 偏移
    // 而略有不同。差异实测约几十像素，表现为“影子拖在物体一侧”。
    // 本片所有成员的 `y` 取相同值（即无纵深偏移），所以两者一致。
    // 若将来给成员加纵深，先给阴影补上同样的透视投影，否则一定会错位。
    const float box_cx = state.x + state.w * 0.5f;
    // ## 阴影必须画在物体的**实际落地处**，不是图层矩形底边（实测）
    //
    // 三维体只占图层矩形的中间约 62%（`model_scale` 与相机距离共同决定），
    // 矩形的底边远低于物体的脚。直接拿 `state.y + state.h` 画阴影，
    // 影就会落在物体下方留白的区域里——实测看起来是两个不相干的物体
    //（一个球，和它下方一个黑色椭圆）。
    //
    // 这里按“物体占矩形中部”的经验比例把影子抬到它真实的落脚处。
    // 比例不是拍脑袋：三维体中心与矩形中心重合，竖直方向约占 0.62，
    // 所以底边在 `中心 + 0.31*h`。
    const float box_bottom = state.y + state.h * 0.5f + state.h * 0.31f;
    // 离地高度 → 影子变淡变大（两个正交变化，缺一个都假）
    const float lift = std::max(0.0f, -bounce_offset(*p.layer, local_time));
    const float height_ratio = std::clamp(lift / 90.0f, 0.0f, 1.0f);
    const float strength = p.layer->model_shadow * (1.0f - 0.6f * height_ratio);
    const float grow = 1.0f + 0.45f * height_ratio;
    // 椭圆：宽 = 图标宽 × 0.42 × grow —— 系数实测决定了“影子”还是“两个物体”
    //
    // 早先取 0.62，阴影直径达到图标的 1.24 倍，在深底上看起来像一块
    // 旁边独立的黑椭圆；0.42 才读成“紧贴脚下一层”。
    const float rx = state.w * 0.42f * grow;
    const float ry = rx * 0.40f;
    const st::math::Color shadow_rgb = p.layer->model_shadow_color;
    // 多层递减：圆心浓、向外淡（近似高斯，层数 5 足够看不出台阶）
    constexpr int kLayers = 5;
    for (int layer_index = kLayers; layer_index >= 1; --layer_index) {
      const float t = static_cast<float>(layer_index) / static_cast<float>(kLayers);
      // 越往外（t 越大）半径越大、alpha 越小
      const float radius_x = rx * t;
      const float radius_y = ry * t;
      const float layer_alpha = strength * (1.0f - t * 0.85f) * 0.9f;
      // 用多个圆拟合椭圆（横向 7 个圆，间距密一点才不会看出“泡”状）
      for (int offset_index = -3; offset_index <= 3; ++offset_index) {
        const float ox = static_cast<float>(offset_index) * radius_x * 0.30f;
        const float local_alpha =
            layer_alpha * (1.0f - 0.12f * std::abs(static_cast<float>(offset_index)));
        if (local_alpha <= 0.004f) continue;
        // 圆心比图标底边**高一点**（让阴影被物体压住一部分，形成接触感）
        canvas.fill_circle(
            st::math::Point{box_cx + ox, box_bottom - state.h * 0.03f}, radius_y,
            st::raster::Paint::solid(st::math::Color{
                shadow_rgb.r, shadow_rgb.g, shadow_rgb.b,
                static_cast<std::uint8_t>(std::lround(
                    std::clamp(local_alpha, 0.0f, 1.0f) * 255.0f))}));
      }
    }
  }
}

}  // namespace caiyun
