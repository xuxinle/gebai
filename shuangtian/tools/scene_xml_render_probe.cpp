// 场景 XML 多层次渲染探针：一份 XML 描述**带层级与可见性**的多层场景。
//
// ## 判据
//
// 1. **层级真实生效**：多层嵌套（机器人 4 层）渲染出实体像素；
//    转动父分组，子零件跟着动（父动子动 = Scene Graph 的定义）。
// 2. **可见性继承真实生效**：`visible="false"` 的分组，其**整棵子树**
//    从画面消失（实体像素显著减少），且不消失兄弟分支的像素。
// 3. **确定性**：同一份 XML 渲两次逐像素相同。
#include <cmath>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_xml.hpp"

namespace {
using namespace st::raster;
using st::math::Color;

constexpr int kW = 480;
constexpr int kH = 360;

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

/// 实体像素占比（与画面背景自适应比较：左上角像素当背景参考）。
auto inked_ratio(const Canvas& canvas) -> double {
  const Color bg = canvas.pixel_at(0, 0);
  int n = 0;
  const int total = canvas.physical_width() * canvas.physical_height();
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      const int d = (static_cast<int>(c.r) - bg.r) * (static_cast<int>(c.r) - bg.r) +
                    (static_cast<int>(c.g) - bg.g) * (static_cast<int>(c.g) - bg.g) +
                    (static_cast<int>(c.b) - bg.b) * (static_cast<int>(c.b) - bg.b);
      if (d > 100) ++n;
    }
  }
  return total > 0 ? static_cast<double>(n) / total : 0.0;
}

/// 两张图的差异像素数（隐藏子树的验证用差分而不是占比：地面/阴影铺满
/// 全屏时 inked 区分度不够，而差分恰好框性"隐藏了什么"）。
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

/// 一棵 4 层的场景树：机器人（躯干 → 肩分组 → 手臂 → 手）+ 左右两条臂。
/// 左臂分组标了 `visible="false"`——验证子树继承隐藏。
constexpr const char* kRobotXml = R"XML(<Scene>
  <Camera eye="0, 1.1, 3.6" target="0, 0.7, 0" fov="40"/>
  <Lighting light_from="0.45, 0.85, 0.55" ambient="0.22" diffuse="0.65" rim="0.2"/>
  <Shadow strength="0.38" ground_y="0"/>
  <Plane name="floor" size="7, 7" ground="true" color="#2a2e36"/>
  <Group name="robot" translate="0, 0, 0">
    <Box name="torso" size="0.62, 0.85, 0.36" color="#4a7dc4" translate="0, 0.72, 0"/>
    <Sphere name="head" size="0.42" color="#c8a07a" translate="0, 1.42, 0"/>
    <Group name="shoulder_L" visible="false" translate="-0.42, 1.05, 0">
      <Cylinder name="upperArm_L" size="0.12, 0.55" color="#9aa7bd" translate="0, -0.24, 0"/>
      <Group name="elbow_L" translate="0, -0.5, 0">
        <Cylinder name="forearm_L" size="0.1, 0.5" color="#9aa7bd" translate="0, -0.22, 0"/>
        <Sphere name="hand_L" size="0.16" color="#c8a07a" translate="0, -0.52, 0"/>
      </Group>
    </Group>
    <Group name="shoulder_R" translate="0.42, 1.05, 0" rotation="0, 0, -12">
      <Cylinder name="upperArm_R" size="0.12, 0.55" color="#9aa7bd" translate="0, -0.24, 0"/>
      <Group name="elbow_R" translate="0, -0.5, 0">
        <Cylinder name="forearm_R" size="0.1, 0.5" color="#9aa7bd" translate="0, -0.22, 0"/>
        <Sphere name="hand_R" size="0.16" color="#c8a07a" translate="0, -0.52, 0"/>
      </Group>
    </Group>
    <Cylinder name="leg_L" size="0.14, 0.62" color="#5b6472" translate="-0.17, 0.31, 0"/>
    <Cylinder name="leg_R" size="0.14, 0.62" color="#5b6472" translate="0.17, 0.31, 0"/>
  </Group>
</Scene>)XML";

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const Color background{22, 24, 32, 255};
  int failures = 0;

  // ── 1. 解析与层级核对 ──
  st::log::out("── 1. 多层场景解析 ──\n");
  const auto spec = parse_scene_xml(kRobotXml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  st::log::out("  顶层对象 {} 个\n", spec->objects.size());
  for (const auto& obj : spec->objects) {
    st::log::out("    <{} name={}> children={}\n", obj.shape, obj.name,
              obj.children.size());
  }
  if (spec->objects.size() < 2) ++failures;

  // ── 2. 渲染：多层嵌套出实体像素 ──
  st::log::out("\n── 2. 多层渲染 ──\n");
  Canvas canvas{kW, kH, 1.0f};
  const auto status = render_scene(*spec, canvas, background);
  if (!status) {
    st::log::out("渲染失败：{}\n", status.error().message);
    return 1;
  }
  // 背景取实际画面的左上角（渲染链含色调映射，写死常量会失真）。
  const double inked_full = inked_ratio(canvas);
  st::log::out("  实体像素占比 {:.3f}（右臂 + 躯干 + 头 + 双腿 + 地面）\n", inked_full);
  if (inked_full < 0.25) ++failures;
  save_png(canvas, out_dir + "/scene-xml-multilayer.png");

  // ── 3. 可见性继承：左臂整棵子树消失 ──
  st::log::out("\n── 3. 可见性继承 ──\n");
  // 把 shoulder_L 的 visible 改回 true（同一份 XML 只动一个字面值）。
  std::string both_arms = kRobotXml;
  const auto pos = both_arms.find("visible=\"false\"");
  if (pos == std::string::npos) {
    st::log::out("  [X] 测试脚本自身错误：找不到 visible=\"false\"\n");
    ++failures;
  } else {
    both_arms.replace(pos, 16, "visible=\"true\" ");
    const auto spec2 = parse_scene_xml(both_arms);
    if (!spec2) {
      st::log::out("  解析失败：{}\n", spec2.error().message);
      ++failures;
    } else {
      Canvas canvas2{kW, kH, 1.0f};
      if (!render_scene(*spec2, canvas2, background)) {
        st::log::out("  渲染失败\n");
        ++failures;
      } else {
        // 差分：双臂 vs 只右臂，差异必须>0（左臂子树消失）且有限（没隐藏别的）。
        const int changed = diff_pixels(canvas2, canvas);
        const int total = kW * kH;
        st::log::out("  双臂 vs 只右臂：{} / {} 像素不同（左臂子树的 footprint）\n",
                  changed, total);
        save_png(canvas2, out_dir + "/scene-xml-both-arms.png");
        if (changed < 200) {
          st::log::out("  [X] visible=false 没有隐藏子树（像素几乎没变）\n");
          ++failures;
        }
        if (changed > total / 2) {
          st::log::out("  [X] 隐藏左臂吃掉了过多像素（隐藏了不该隐藏的）\n");
          ++failures;
        }
      }
    }
  }

  // ── 4. 父动子动：转动 robot 分组，画面必须变 ──
  st::log::out("\n── 4. 父动子动 ──\n");
  {
    std::string rotated = both_arms.empty() ? std::string(kRobotXml) : both_arms;
    const auto gpos = rotated.find("<Group name=\"robot\"");
    const auto insert_at = rotated.find('>', gpos);
    rotated.insert(insert_at, " rotation=\"0, 28, 0\"");
    const auto spec3 = parse_scene_xml(rotated);
    if (!spec3) {
      st::log::out("  解析失败：{}\n", spec3.error().message);
      ++failures;
    } else {
      Canvas canvas3{kW, kH, 1.0f};
      if (!render_scene(*spec3, canvas3, background)) {
        st::log::out("  渲染失败\n");
        ++failures;
      } else {
        const bool changed = diff_pixels(canvas, canvas3) > 0;
        st::log::out("  转动父分组后画面改变：{}\n", changed ? "是" : "否");
        if (!changed) ++failures;
        save_png(canvas3, out_dir + "/scene-xml-rotated.png");
      }
    }
  }

  // ── 5. 确定性 ──
  st::log::out("\n── 5. 确定性 ──\n");
  {
    Canvas first{kW, kH, 1.0f};
    Canvas again{kW, kH, 1.0f};
    if (render_scene(*spec, first, background) && render_scene(*spec, again, background)) {
      const bool same = diff_pixels(first, again) == 0;
      st::log::out("  同一份 XML 渲两次逐像素相同：{}\n", same ? "是" : "否");
      if (!same) ++failures;
    }
  }

  st::log::out("\n{}\n", failures == 0 ? "全部通过" : "有 " + std::to_string(failures) + " 项失败");
  return failures == 0 ? 0 : 1;
}
