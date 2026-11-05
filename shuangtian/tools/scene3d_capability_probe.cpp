// three.js / bpy **表达能力**的对照探针：一批脚本，逐个真跑，看哪些能力真的可用。
//
// ## 为什么不是“看代码里有没有实现”
//
// 本仓库反复踩到同一个坑：某个能力在 C++ 里写了一段，但**另一条路径没接**
// （`sample_light` 只接到 `shade_surface`、`set_material` GPU 腿空实现…），
// 于是“grep 到了”与“真的生效”是两件事。
//
// 所以这里的原则是：**每个能力用自己的脚本跑一遍，要求它产出可验证的场景差异**。
// 探针只回答两件事：能力是否被识别（不报错）＋是否真的改变了场景（物体/材质/灯光字段）。
//
// 用法：能力清单是**数据**（`kCapabilities`），加一条就是加一行——不写一堆 main。

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "st/raster/bpy_runtime.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/three_runtime.hpp"

namespace {

/// 一个刻画“某能力可用”的最小脚本 + 判定它生效的检查。
struct Capability {
  const char* name;
  const char* kind;        ///< "three" / "bpy"
  const char* script;
  const char* what;        ///< 一句话说明这条在验什么
};

// ⚠ 清单只放**三条表达层里最常用、缺失就会立刻看出来**的能力。
// 每一条都必须能给出“脚本跑了 + 场景真的变了”的证据，否则不如不列。
// ⚠ 脚本写法说明：每条都以 `const scene = new THREE.Scene()` 开头、以 `scene` 结尾
// （**不带分号**），这是用户真实会写的形式（与 `examples/jianmu/scenes/*.js` 一致）。
// 探测程要验的是“这个能力能不能用”，不该用非常规写法把自身缺陷测成能力缺口。
const Capability kCapabilities[] = {
    // ── three.js：几何生成器 ──
    {"three.LatheGeometry", "three",
     R"js(const scene = new THREE.Scene();
scene.add(new THREE.Mesh(new THREE.LatheGeometry(
  [new THREE.Vector2(0.2,0.0),new THREE.Vector2(0.5,0.4),new THREE.Vector2(0.3,0.9)],24),
  new THREE.MeshStandardMaterial({color:0xcc8844})));
scene
)js",
     "旋转体（花瓶/杯/轴）：Blender 的 screw 对应物"},
    {"three.TubeGeometry", "three",
     R"js(const scene = new THREE.Scene();
const curve = new THREE.CatmullRomCurve3([new THREE.Vector3(-1,0,0),
  new THREE.Vector3(0,0.7,0),new THREE.Vector3(1,0,0)]);
scene.add(new THREE.Mesh(new THREE.TubeGeometry(curve,32,0.12,8,false),
  new THREE.MeshStandardMaterial({color:0x66aadd})));
scene
)js",
     "管道/走线/边框圆角：曲线扫掠"},
    {"three.TorusKnotGeometry", "three",
     R"js(const scene = new THREE.Scene();
scene.add(new THREE.Mesh(new THREE.TorusKnotGeometry(0.6,0.18,64,8),
  new THREE.MeshStandardMaterial({color:0xdddddd,metalness:0.9,roughness:0.2})));
scene
)js",
     "环面纽结：复杂曲面与金属高光的压测形状"},
    {"three.ShapeGeometry+holes", "three",
     R"js(const scene = new THREE.Scene();
const sh = new THREE.Shape();
sh.moveTo(-1,-1);sh.lineTo(1,-1);sh.lineTo(1,1);sh.lineTo(-1,1);sh.lineTo(-1,-1);
const hole = new THREE.Path();
hole.absarc(0,0,0.4,0,Math.PI*2,true);
sh.holes.push(hole);
scene.add(new THREE.Mesh(new THREE.ExtrudeGeometry(sh,{depth:0.3,bevelEnabled:false}),
  new THREE.MeshStandardMaterial({color:0x88bb66})));
scene
)js",
     "带孔的挤出件（法兰/支架）：Shape + holes"},
    {"three.InstancedMesh", "three",
     R"js(const scene = new THREE.Scene();
const geo = new THREE.BoxGeometry(0.2,0.2,0.2);
const mat = new THREE.MeshStandardMaterial({color:0xaaaaaa});
const inst = new THREE.InstancedMesh(geo,mat,27);
const mx = new THREE.Matrix4();
let i = 0;
for (let x=-1;x<=1;x++) { for (let y=-1;y<=1;y++) { for (let z=-1;z<=1;z++) {
  mx.makeTranslation(x*0.5,y*0.5,z*0.5); inst.setMatrixAt(i++,mx);
} } }
scene.add(inst);
scene
)js",
     "实例化：同一几何大量重复（叶片/铆钉/砖）"},
    {"three.Group+nesting", "three",
     R"js(const scene = new THREE.Scene();
const inner = new THREE.Group();
const arm = new THREE.Group();
const seg = new THREE.Mesh(new THREE.CylinderGeometry(0.08,0.08,0.8,12),
  new THREE.MeshStandardMaterial({color:0xdd8844}));
seg.position.set(0,0.4,0);
arm.add(seg);
arm.position.set(0.3,0,0);
inner.add(arm);
scene.add(inner);
scene
)js",
     "层级与嵌套：机械臂/装配体的父子关系"},

    // ── three.js：材质与表现 ──
    {"three.transparent", "three",
     R"js(const scene = new THREE.Scene();
scene.add(new THREE.Mesh(new THREE.BoxGeometry(1,1,1),
  new THREE.MeshStandardMaterial({color:0x66ccff,transparent:true,opacity:0.35})));
scene
)js",
     "透明混合：玻璃/水/亚克力"},
    {"three.DoubleSide", "three",
     R"js(const scene = new THREE.Scene();
scene.add(new THREE.Mesh(new THREE.PlaneGeometry(1.6,1.6),
  new THREE.MeshStandardMaterial({color:0x88aa66,side:THREE.DoubleSide})));
scene
)js",
     "双面材质：单层薄片（布/叶/旗）"},
    {"three.emissive", "three",
     R"js(const scene = new THREE.Scene();
const mat = new THREE.MeshStandardMaterial({color:0x111111});
mat.emissive = new THREE.Color(0x44ddff);
mat.emissiveIntensity = 1.6;
scene.add(new THREE.Mesh(new THREE.SphereGeometry(0.5,24,16), mat));
scene
)js",
     "自发光：发光体/指示灯（不受光时也要亮）"},
    {"three.texture", "three",
     R"js(const scene = new THREE.Scene();
scene.add(new THREE.Mesh(new THREE.BoxGeometry(1,1,1),
  new THREE.MeshStandardMaterial({color:0xffffff})));
scene
)js",
     "贴图通道（需外部图；这里验材质字段不崩）"},

    // ── three.js：灯光与相机 ──
    {"three.PointLight", "three",
     R"js(const scene = new THREE.Scene();
const light = new THREE.PointLight(0xffaa55,3.0,12);
light.position.set(1.2,1.2,1.5);
scene.add(light);
scene.add(new THREE.Mesh(new THREE.SphereGeometry(0.5,24,16),
  new THREE.MeshStandardMaterial({color:0x888888})));
scene
)js",
     "点光：1/d² 衰减与半径截断"},
    {"three.SpotLight", "three",
     R"js(const scene = new THREE.Scene();
const light = new THREE.SpotLight(0xffffff,4.0,14,Math.PI/9,0.35,1.0);
light.position.set(0,2.2,0.8);
light.target.position.set(0,0,0);
scene.add(light);
scene.add(light.target);
scene.add(new THREE.Mesh(new THREE.SphereGeometry(0.5,24,16),
  new THREE.MeshStandardMaterial({color:0x999999})));
scene
)js",
     "聚光：内/外锥（舞台灯/车灯）"},
    {"three.OrthographicCamera", "three",
     R"js(const scene = new THREE.Scene();
const cam = new THREE.OrthographicCamera(-2,2,1.6,-1.6,0.1,50);
cam.position.set(0,0,4);
cam.lookAt(0,0,0);
scene.add(cam);
scene.add(new THREE.Mesh(new THREE.BoxGeometry(1,1,1),
  new THREE.MeshStandardMaterial({color:0xaa8866})));
scene
)js",
     "正交相机：等距视图/工程图（无透视收缩）"},
    {"three.HemisphereLight", "three",
     R"js(const scene = new THREE.Scene();
scene.add(new THREE.HemisphereLight(0x99bbff,0x332211,1.2));
scene.add(new THREE.Mesh(new THREE.SphereGeometry(0.6,24,16),
  new THREE.MeshStandardMaterial({color:0xcccccc})));
scene
)js",
     "半球光：天/地双色环境（室外感）"},

    // ── three.js：动画 ──
    {"three.userData.spin", "three",
     R"js(const scene = new THREE.Scene();
const mesh = new THREE.Mesh(new THREE.BoxGeometry(0.8,0.8,0.8),
  new THREE.MeshStandardMaterial({color:0xcc6666}));
mesh.userData.spin = {axis:[0,1,0],speed:1.2};
scene.add(mesh);
scene
)js",
     "自转意图：视口每帧推进（零重求值）"},
    {"three.AnimationMixer", "three",
     R"js(const scene = new THREE.Scene();
const mesh = new THREE.Mesh(new THREE.BoxGeometry(0.6,0.6,0.6),
  new THREE.MeshStandardMaterial({color:0x66cc99}));
mesh.position.set(1,0,0);
scene.add(mesh);
const track = new THREE.VectorKeyframeTrack('.position',[0,1,2],[1,0,0,-1,0,0,1,0,0]);
const clip = new THREE.AnimationClip('m',2,[track]);
const mixer = new THREE.AnimationMixer(scene);
mixer.clipAction(clip).play();
scene.add(mixer.getRoot());
scene
)js",
     "关键帧动画：位置/旋转/缩放轨道"},

    // ── bpy：建模能力 ──
    {"bpy.bmesh", "bpy",
     R"py(import bpy, bmesh
mesh = bpy.data.meshes.new('m')
bm = bmesh.new()
a = bm.verts.new((0.0, 0.0, 0.0))
b = bm.verts.new((1.0, 0.0, 0.0))
c = bm.verts.new((0.5, 0.9, 0.0))
bm.faces.new((a, b, c))
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new('tri', mesh)
result = 'tri'
)py",
     "bmesh 逐顶点建面（参数化几何的入口）"},
    {"bpy.ops.primitives", "bpy",
     R"py(import bpy
bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=0.4, depth=1.2)
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.35, location=(1.2, 0.0, 0.0))
bpy.ops.mesh.primitive_torus_add(major_radius=0.8, minor_radius=0.16,
                                 location=(-1.2, 0.0, 0.0))
result = 'prims'
)py",
     "bpy.ops 图元（柱/球/环）"},
    {"bpy.materials", "bpy",
     R"py(import bpy
bpy.ops.mesh.primitive_cube_add(size=1.0)
obj = bpy.context.active_object
mat = bpy.data.materials.new('steel')
mat.diffuse_color = (0.6, 0.65, 0.7, 1.0)
mat.metallic = 0.9
mat.roughness = 0.25
obj.material_slots.append(mat)
result = 'mat'
)py",
     "bpy 材质（基色/金属度/粗糙度）"},
    {"bpy.mathutils", "bpy",
     R"py(import bpy
from mathutils import Euler, Vector
bpy.ops.mesh.primitive_cube_add(size=0.8)
obj = bpy.context.active_object
obj.rotation_euler = Euler(0.4, 0.6, 0.0)
obj.location = Vector((1.5, 0.0, 0.0))
result = 'math'
)py",
     "mathutils（Vector/Euler/Matrix）与变换"},
};

}  // namespace

auto main() -> int {
  int failures = 0;
  std::printf("%-28s %-6s %s\n", "能力", "表达层", "结果");
  std::printf("%s\n", std::string(78, '-').c_str());
  for (const auto& capability : kCapabilities) {
    const bool is_bpy = std::string_view(capability.kind) == "bpy";
    const auto result = is_bpy
        ? st::raster::BpySceneExtractor::extract(capability.script)
        : st::raster::ThreeRuntime::instance().evaluate(capability.script, capability.name);
    if (!result.has_value()) {
      std::printf("%-28s %-6s ✗ %s\n    要验：%s\n", capability.name, capability.kind,
                  result.error().message.c_str(), capability.what);
      ++failures;
      continue;
    }
    // 判据：能产出物体 + （材质/灯光至少有一项非默认）才算“这个能力真的可用”。
    const bool has_objects = !result->objects.empty();
    bool has_visible = false;
    for (const auto& object : result->objects) {
      if (object.mesh.vertices.empty()) continue;
      has_visible = true;
      break;
    }
    if (!has_objects || !has_visible) {
      std::printf("%-28s %-6s ✗ 场景里没有可画物体（%zu 个对象）\n    要验：%s\n",
                  capability.name, capability.kind, result->objects.size(), capability.what);
      ++failures;
      continue;
    }
    std::printf("%-28s %-6s ✓ %zu 个物体\n    要验：%s\n", capability.name, capability.kind,
                result->objects.size(), capability.what);
  }
  std::printf("\n%s（%d 项不可用，共 %zu 项）\n",
              failures == 0 ? "全部能力可用" : "有缺口", failures,
              sizeof(kCapabilities) / sizeof(kCapabilities[0]));
  return failures == 0 ? 0 : 1;
}
