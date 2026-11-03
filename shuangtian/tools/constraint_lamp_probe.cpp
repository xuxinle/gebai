// 约束系统渲染验证：同一台「台灯」场景，约束版 vs 手摆版对照 + 改台面高度的联动验证。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_xml.hpp"

namespace {
using namespace st::raster;
using st::math::Color;

constexpr int kW = 480;
constexpr int kH = 420;

auto save_png(const Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

auto diff_pixels(const Canvas& a, const Canvas& b) -> int {
  const auto pa = a.pixels();
  const auto pb = b.pixels();
  if (pa.size() != pb.size()) return -1;
  int n = 0;
  for (std::size_t i = 0; i < pa.size(); ++i) {
    if (pa[i] != pb[i]) ++n;
  }
  return n;
}

/// 约束版台灯：所有零件用 attach/connect 声明装配关系。
/// table_h 参数只改台面高度——立柱/灯头/电缆必须全部跟随（这就是约束的意义）。
auto lamp_scene(float table_y) -> std::string {
  char buffer[2048];
  std::snprintf(buffer, sizeof(buffer), R"XML(<Scene>
  <Camera eye="1.8, 1.6, 2.6" target="0, %.2f, 0" fov="40"/>
  <Lighting light_from="0.5, 0.8, 0.5" ambient="0.2" env_strength="0.8" env_sky="#87a7d4"/>
  <Shadow strength="0.35" ground_y="0"/>
  <Box name="table" size="1.6, %.2f, 1.6" translate="0, %.2f, 0" color="#5a4a3a"/>
  <Cylinder name="post" size="0.07, 0.9" attach="top_of:table" color="#8c8c94"
            metallic="0.9" roughness="0.3" specular="1"/>
  <Sphere name="shade" size="0.34" attach="top_of:post+0, 0.02, 0" color="#e8dcc0"
          roughness="0.5"/>
  <Sphere name="bulb" size="0.16" attach="center_of:shade+0, -0.05, 0" color="#fff4d0"
          emission="2.0" emissive_color="#ffe9b0" emission_unlit="true"/>
  <Tube name="cable" connect="bottom_of:shade -> front_of:table" size="0.03, auto"
        color="#333340"/>
  <Plane name="floor" size="7, 7" ground="true" color="#2b2320"/>
</Scene>)XML",
                double(table_y) + 0.3, 0.12, double(table_y));
  return buffer;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const Color background{18, 17, 22, 255};
  int failures = 0;

  // ── 1. 标准台面（y=0.45）──
  const auto spec = parse_scene_xml(lamp_scene(0.45f));
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  st::log::out("生成 XML 长度 {} 字节\n", lamp_scene(0.45f).size());
  Canvas canvas{kW, kH, 1.0f};
  if (!render_scene(*spec, canvas, background)) {
    st::log::out("渲染失败\n");
    return 1;
  }
  save_png(canvas, out_dir + "/constraint-lamp-h045.png");
  st::log::out("标准台面已出图\n");

  // 数值断言：装配链
  // table top = 0.45+0.06 = 0.51; post(高0.9) center = 0.51+0.45 = 0.96
  // post top = 1.41; shade center = 1.41+0.02+0.17 = 1.60
  const auto find = [&](const char* name) -> const SceneObjectSpec* {
    for (const auto& object : spec->objects) {
      if (object.name == name) return &object;
    }
    return nullptr;
  };
  const auto* post = find("post");
  const auto* shade = find("shade");
  const auto* cable = find("cable");
  const auto near = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
  if (post == nullptr || shade == nullptr || cable == nullptr) {
    st::log::out("[X] 零件缺失（共 {} 个顶层对象）\n", spec->objects.size());
    for (const auto& object : spec->objects) {
      st::log::out("    - {} attach=\"{}\" connect=\"{}\"\n", object.name,
               object.attach, object.connect);
    }
    ++failures;
  } else {
    if (!near(post->translate.y, 0.96f)) {
      st::log::out("[X] post.y={} 期望 0.96\n", double(post->translate.y));
      ++failures;
    } else {
      st::log::out("[ok] post.y = 0.96（贴台面 + 半高）\n");
    }
    if (!near(shade->translate.y, 1.60f)) {
      st::log::out("[X] shade.y={} 期望 1.60\n", double(shade->translate.y));
      ++failures;
    } else {
      st::log::out("[ok] shade.y = 1.60（柱顶 + 偏移 + 半径）\n");
    }
    // cable 长度 = shade bottom (1.43) 到 table front (0,0.45,0.8) 的距离
    const float expected =
        std::sqrt(0.0f + (1.43f - 0.45f) * (1.43f - 0.45f) + 0.8f * 0.8f);
    if (!near(cable->size.y, expected)) {
      st::log::out("[X] cable 长度={} 期望 {}\n", double(cable->size.y),
                   double(expected));
      ++failures;
    } else {
      st::log::out("[ok] cable 自动长度 = {:.4f}\n", double(cable->size.y));
    }
  }

  // ── 2. 台面升高 0.3：整台灯必须联动（约束的意义所在）──
  const auto spec_high = parse_scene_xml(lamp_scene(0.75f));
  if (!spec_high) {
    st::log::out("高台面解析失败：{}\n", spec_high.error().message);
    ++failures;
  } else {
    const auto* post_high = [&] {
      for (const auto& object : spec_high->objects) {
        if (object.name == "post") return &object;
      }
      return static_cast<const SceneObjectSpec*>(nullptr);
    }();
    if (post_high == nullptr || !near(post_high->translate.y, 0.96f + 0.30f)) {
      st::log::out("[X] 台面 +0.3 后 post.y={} 期望 1.26（联动失败）\n",
               post_high == nullptr ? -1.0 : double(post_high->translate.y));
      ++failures;
    } else {
      st::log::out("[ok] 台面 +0.3 → post.y 自动跟到 1.26（联动成立）\n");
    }
    Canvas canvas_high{kW, kH, 1.0f};
    if (render_scene(*spec_high, canvas_high, background)) {
      save_png(canvas_high, out_dir + "/constraint-lamp-h075.png");
      const int changed = diff_pixels(canvas, canvas_high);
      st::log::out("[ok] 高台面渲染已出图（{} 像素变化）\n", changed);
    }
  }

  // ── 3. 错误报路：前向引用必须被拒 ──
  const auto bad = parse_scene_xml(
      "<Scene><Sphere name=\"a\" attach=\"top_of:b\"/><Box name=\"b\"/></Scene>");
  if (bad.has_value()) {
    st::log::out("[X] 前向引用（引用后出现的名字）居然成功\n");
    ++failures;
  } else {
    const std::string message = bad.error().message;
    const bool located = message.find("b") != std::string::npos;
    st::log::out("[{}] 前向引用被拒：{}\n", located ? "ok" : "X", message);
    if (!located) ++failures;
  }

  // ── 4. 互斥检查 ──
  const auto both = parse_scene_xml(
      "<Scene><Box name=\"t\"/><Sphere name=\"a\" attach=\"top_of:t\" "
      "align=\"axis:t\"/></Scene>");
  if (both.has_value()) {
    st::log::out("[X] attach+align 同时写居然成功\n");
    ++failures;
  } else {
    st::log::out("[ok] 互斥拒绝：{}\n", both.error().message);
  }

  st::log::out("\n{}\n", failures == 0 ? "全部通过" : "有 " + std::to_string(failures) + " 项失败");
  return failures == 0 ? 0 : 1;
}
