#include "usd_export.hpp"

#include <algorithm>
#include <format>
#include <map>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/mesh.hpp"

namespace caiyun {
namespace {

using st::ErrorCode;

/// USD 要求标识符合法（字母开头，只含字母/数字/下划线）。
///
/// 对象的 `name` / `shape` 常常含 `-`、空格、中文。直接拿来做 Prim 名
/// 会让 `usdcat` 报语法错——而错误信息指向"第几行有非法字符"，
/// 离"是某个对象名"很远。所以在这里统一净化。
[[nodiscard]] auto sanitize_identifier(std::string_view raw, std::string_view fallback)
    -> std::string {
  std::string out;
  out.reserve(raw.size() + 1);
  for (const char ch : raw) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_';
    out.push_back(ok ? ch : '_');
  }
  if (out.empty()) return std::string{fallback};
  // 首字符不能是数字
  if (out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), '_');
  return out;
}

/// 浮点数写成 USD 能读的形式。
///
/// 为何不直接 `{}`：`std::format("{}", 1.0f)` 得到 `1`（USD 会当 int 解析），
/// 而 `1.0f` 的十六进制浮点表示 USD 读不了。这里固定用定点 6 位并去掉尾零，
/// 保证既有小数点、又不带科学计数法（`1e-06` 在 usda 里合法但可读性差）。
[[nodiscard]] auto num(double value) -> std::string {
  std::string text = std::format("{:.6f}", value);
  // 去掉尾零，但保留小数点后至少一位
  const std::size_t dot = text.find('.');
  if (dot != std::string::npos) {
    std::size_t last = text.find_last_not_of('0');
    if (last == dot) last = dot + 1;  // 至少留一位
    text.erase(last + 1);
  }
  return text;
}

[[nodiscard]] auto vec3(const st::math::Vec3& v) -> std::string {
  return std::format("({}, {}, {})", num(v.x), num(v.y), num(v.z));
}

/// sRGB 颜色 → USD 线性色。
///
/// 霜天的 `math::Color` 是 sRGB 字节值（8 位），而 USD 的 `color3f` 期望**线性**。
/// 不转换会让整体偏亮（gamma 差异），且难查——两边都是"看起来是灰色"。
[[nodiscard]] auto to_linear(double srgb_byte) -> double {
  const double s = srgb_byte / 255.0;
  return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

[[nodiscard]] auto color3f(const st::math::Color& c) -> std::string {
  return std::format("({}, {}, {})", num(to_linear(c.r)), num(to_linear(c.g)),
                     num(to_linear(c.b)));
}

/// 写一个 `mesh` Prim 的 points / faceVertexCounts / faceVertexIndices。
struct MeshData {
  std::vector<st::math::Vec3> points{};
  std::vector<int> counts{};
  std::vector<int> indices{};
};

/// 从网格顶点与索引里抽出 USD 需要的三组数组。
///
/// 为什么需要转换：霜天的 `Mesh` 是**渲染器友好的**紧凑数组
///（每顶点 9 个 float：位置 3 + 法线 3 + 颜色 3），索引另存；
/// USD 的 `Mesh` 要的是 `points` + `faceVertexCounts` + `faceVertexIndices`。
[[nodiscard]] auto mesh_to_usd_data(const st::raster::Mesh& mesh) -> MeshData {
  MeshData out{};
  const std::size_t count = mesh.vertex_count();
  out.points.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t at = i * st::raster::Mesh::kVertexFloats;
    out.points.push_back(st::math::Vec3{mesh.vertices[at + 0], mesh.vertices[at + 1],
                                        mesh.vertices[at + 2]});
  }
  // 三角形：索引**每 3 个一组**就是一个面。
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    out.counts.push_back(3);
    out.indices.push_back(static_cast<int>(mesh.indices[i + 0]));
    out.indices.push_back(static_cast<int>(mesh.indices[i + 1]));
    out.indices.push_back(static_cast<int>(mesh.indices[i + 2]));
  }
  return out;
}

}  // namespace

auto scene_spec_to_usda(const st::raster::SceneSpec& spec, const UsdExportOptions& options)
    -> Result<std::string> {
  std::string out;
  const auto line = [&](std::string_view text) { out += text; out += '\n'; };

  line("#usda 1.0");
  line("(");
  line("    defaultPrim = \"" + sanitize_identifier(options.root_name, "World") + "\"");
  // 说明来源：一份 usda 进了下游管线之后，没人知道它是哪来的、由什么版本产生。
  line(std::format("    doc = \"Exported by caiyun (shuangtian {})\"", ST_VERSION));
  line("    metersPerUnit = 1");
  // Y-up：与霜天三维同向，不做轴变换（见头文件说明）。
  line("    upAxis = \"Y\"");
  line(")");
  line("");

  const std::string root = sanitize_identifier(options.root_name, "World");
  line(std::format("def Xform \"{}\"", root));
  line("{");

  // ## 材质：一份共用
  //
  // 每个对象各写一份材质会让文件膨胀十几倍，且改一个颜色要改十几处。
  // 代价：同一个场景里只能有一个材质（对纯色演示够用；要分材质得扩成
  // 每对象一份并去重——留待真需要时再做，而不是先做一套"可能的"抽象）。
  line(std::format("    def Material \"{}\"", options.material_name));
  line("    {");
  line("        def Shader \"PreviewSurface\"");
  line("        {");
  line("            uniform token info:id = \"UsdPreviewSurface\"");
  line("            color3f inputs:diffuseColor = (0.6, 0.6, 0.6)");
  line("            float inputs:roughness = 0.6");
  line("            float inputs:metallic = 0");
  line("            token outputs:surface");
  line("        }");
  line("    }");
  line("");

  int index = 0;
  for (const auto& object : spec.objects) {
    // 生成网格（`obj:` 在这里被真正读取）。加载失败**整体失败**——
    // 静默少一个物体是导出类功能最难发现的缺陷。
    const auto generated = st::raster::build_mesh(object);
    if (!generated) {
      return st::unexpected(
          generated.error().code,
          std::format("导出时生成对象 {}（{}）的网格失败：{}", index, object.shape,
                      generated.error().message));
    }
    const MeshData data = mesh_to_usd_data(*generated);
    if (data.counts.empty()) {
      ST_LOG_WARN("对象 {}（{}）没有面，已跳过", index, object.shape);
      ++index;
      continue;
    }

    std::string prim = sanitize_identifier(object.shape, "Object");
    prim += std::format("_{}", index);

    line(std::format("    def Mesh \"{}\"", prim));
    line("    {");
    // 变换：先缩放、再旋转、后平移（与渲染器的 TRS 顺序一致）。
    line(std::format("        double3 xformOp:translate = {}", vec3(object.translate)));
    // `object.rotation` 内部已是**弧度**，USD 的 rotateXYZ 要**度**，所以这里转换
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    line(std::format("        float3 xformOp:rotateXYZ = ({}, {}, {})",
                     num(static_cast<double>(object.rotation.x) * kRadToDeg),
                     num(static_cast<double>(object.rotation.y) * kRadToDeg),
                     num(static_cast<double>(object.rotation.z) * kRadToDeg)));
    line(std::format("        float3 xformOp:scale = {}", vec3(object.scale)));
    line("        uniform token[] xformOpOrder = [\"xformOp:translate\", "
         "\"xformOp:rotateXYZ\", \"xformOp:scale\"]");

    // 点与拓扑
    out += "        point3f[] points = [";
    for (std::size_t i = 0; i < data.points.size(); ++i) {
      if (i != 0) out += ", ";
      out += vec3(data.points[i]);
    }
    out += "]\n";

    out += "        int[] faceVertexCounts = [";
    for (std::size_t i = 0; i < data.counts.size(); ++i) {
      if (i != 0) out += ", ";
      out += std::to_string(data.counts[i]);
    }
    out += "]\n";

    out += "        int[] faceVertexIndices = [";
    for (std::size_t i = 0; i < data.indices.size(); ++i) {
      if (i != 0) out += ", ";
      out += std::to_string(data.indices[i]);
    }
    out += "]\n";

    // 材质绑定 + 固有色（用 displayColor 承载：UsdPreviewSurface 的
    // diffuseColor 是共享材料，逐对象的固有色走 displayColor 更接近语义）
    line("        rel material:binding = </" + root + "/" + options.material_name + ">");
    line(std::format("        color3f[] primvars:displayColor = [{}]",
                     color3f(object.color)));
    if (object.opacity < 1.0f) {
      line(std::format("        float primvars:opacity = {}", num(object.opacity)));
    }
    line("    }");
    ++index;
  }
  line("}");

  return out;
}

auto export_usda_from_script_text(std::string_view json_text, const UsdExportOptions& options)
    -> Result<std::string> {
  const auto spec = st::raster::parse_scene_spec_text(json_text);
  if (!spec) {
    return st::unexpected(spec.error().code,
                          std::format("model_script 解析失败：{}", spec.error().message));
  }
  return scene_spec_to_usda(*spec, options);
}

}  // namespace caiyun
