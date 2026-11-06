// 最小闭环：内嵌 MicroPython 能跑一段 Python 并取回结果。
//
// ## 为什么先验这个
//
// 这是整条 bpy 链路的地基。若连"跑 `result = 1 + 1` 取回 2"都做不到，
// 后面的 bpy / bmesh / mathutils 全是空谈。
//
// ## 判据分四层（层层加码）
//
// 1. 语法与算术：`result = 1 + 2` → "3"
// 2. 数据结构：list/dict/for 循环
// 3. 函数与类：bpy 脚本里的常见形态
// 4. 异常：出错时必须给出**可读的原因**，而不是静默返回空
#include <cstdio>
#include <string>

#include "st/python/runtime.hpp"

namespace {

int failures = 0;   // lint-allow: L8 探针的用例计数器（一次性量尺，与 tests/ 同口径）

auto check(const char* label, const char* source, const char* expect_substr) -> void {
  auto& runtime = st::mp::Runtime::instance();
  const auto result = runtime.run(source, "<probe>");
  if (!result.has_value()) {
    std::printf("  [FAIL] %-30s 执行失败：%s\n", label, result.error().message.c_str());
    ++failures;
    return;
  }
  const bool ok = result->value.find(expect_substr) != std::string::npos;
  std::printf("  [%s] %-30s result=%s\n", ok ? "PASS" : "FAIL", label,
              result->value.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main() {
  std::printf("MicroPython 可用：%s\n", st::mp::Runtime::available() ? "是" : "否");
  if (!st::mp::Runtime::available()) {
    std::printf("  原因：%s\n", st::mp::Runtime::unavailable_reason().c_str());
    return 1;
  }
  std::printf("\n");

  // ① 语法与算术
  check("算术", "result = 1 + 2", "3");
  check("浮点", "result = 3.0 / 4", "0.75");
  check("字符串", "result = 'turbo' + 'fan'", "turbofan");

  // ② 数据结构 + 循环（bpy 脚本的基本形态）
  check("list + sum", "xs = [i * 2 for i in range(5)]\nresult = sum(xs)", "20");
  check("dict", "d = {'teeth': 60}\nresult = d['teeth']", "60");
  check("for + 累加",
        "total = 0\nfor i in range(1, 11):\n    total += i\nresult = total", "55");

  // ③ 函数与类（bpy 里到处是 "定义一个造齿轮的函数"）
  check("函数",
        "def pitch(n):\n    return 6.283185 / n\nresult = round(pitch(60), 4)", "0.1047");
  check("类",
        "class Gear:\n"
        "    def __init__(self, teeth):\n"
        "        self.teeth = teeth\n"
        "    def label(self):\n"
        "        return 'z%d' % self.teeth\n"
        "result = Gear(42).label()", "z42");

  // ④ 内建能力：math 是 bpy 脚本重度依赖的
  check("math",
        "import math\nresult = round(math.sin(math.pi / 6), 4)", "0.5");

  // ⑤ 异常必须给出可读原因
  {
    auto& runtime = st::mp::Runtime::instance();
    const auto bad = runtime.run("result = 1 / 0");
    const bool ok = !bad.has_value() && !bad.error().message.empty();
    std::printf("  [%s] %-30s %s\n", ok ? "PASS" : "FAIL", "异常有可读原因",
                bad.has_value() ? "(没报错，不符合预期)" : bad.error().message.c_str());
    if (!ok) ++failures;
  }

  // ⑥ mathutils（bpy 语料的第一行就是它）
  check("mathutils 向量",
        "from mathutils import Vector\n"
        "v = Vector((3, 4, 0))\n"
        "result = v.length",
        "5");
  check("mathutils 运算",
        "from mathutils import Vector\n"
        "a = Vector((1, 2, 3)) + Vector((4, 5, 6))\n"
        "result = a.x",
        "5");
  // `@` 运算符：MicroPython 编译器不支持（PEP 465），所以用显式方法名。
  // 这条边界写进 docs/python.md——不静默改写源码，而是如实告知。
  check("mathutils 点乘（dot）",
        "from mathutils import Vector\n"
        "result = Vector((1, 0, 0)).dot(Vector((2, 3, 4)))",        "2");
  check("mathutils 叉乘",
        "from mathutils import Vector\n"
        "v = Vector((1, 0, 0)).cross(Vector((0, 1, 0)))\n"
        "result = '%g,%g,%g' % (v.x, v.y, v.z)",
        "0,0,1");
  check("mathutils 归一化与属性写",
        "from mathutils import Vector\n"
        "v = Vector((3, 0, 0))\n"
        "v.normalize()\n"
        "v.z = 7\n"
        "result = '%g,%g' % (v.x, v.z)",
        "1,7");

  // ⑦ bpy.data / bpy.ops（真 bpy 脚本的写法）
  check("bpy 建基本体",
        "import bpy\n"
        "bpy.ops.mesh.primitive_cube_add(size=2.0)\n"
        "obj = bpy.context.active_object\n"
        "result = '%s:%d:%d' % (obj.name, len(obj.data.vertices), len(obj.data.polygons))",
        "Cube:8:6");
  check("bpy 圆柱轴沿 Z",
        "import bpy\n"
        "bpy.ops.mesh.primitive_cylinder_add(vertices=8, radius=1.0, depth=2.0)\n"
        "v = bpy.context.active_object.data.vertices[0]\n"
        "result = '%g,%g' % (v.co.x, v.co.z)",
        "1,-1");
  check("bpy 对象变换与矩阵",
        "import bpy\n"
        "from mathutils import Vector\n"
        "bpy.ops.mesh.primitive_plane_add(size=2.0)\n"
        "obj = bpy.context.active_object\n"
        "obj.location = Vector((10, 0, 0))\n"
        "result = obj.matrix_world[0][3]",
        "10");
  check("bpy.data 按名查找",
        "import bpy\n"
        "bpy.ops.mesh.primitive_uv_sphere_add(segments=8, ring_count=4, radius=1.0)\n"
        "result = bpy.data.objects['Sphere'].name",
        "Sphere");

  // ⑧ bmesh（逐顶点建模，齿轮/叶片靠它）
  check("bmesh 建面",
        "import bpy, bmesh\n"
        "mesh = bpy.data.meshes.new('M')\n"
        "bm = bmesh.new()\n"
        "v0 = bm.verts.new((0, 0, 0))\n"
        "v1 = bm.verts.new((1, 0, 0))\n"
        "v2 = bm.verts.new((0, 1, 0))\n"
        "bm.faces.new((v0, v1, v2))\n"
        "bm.to_mesh(mesh)\n"
        "bm.free()\n"
        "result = '%d:%d' % (len(mesh.vertices), len(mesh.polygons))",
        "3:1");

  // ⑨ 命名空间隔离：上一次的 result 不该泄漏到下一次
  {
    auto& runtime = st::mp::Runtime::instance();
    (void)runtime.run("result = 'leaked'");
    const auto fresh = runtime.run("x = 1");
    const bool ok = fresh.has_value() && fresh->value.empty();
    std::printf("  [%s] %-30s result=%s\n", ok ? "PASS" : "FAIL", "命名空间隔离",
                fresh.has_value() ? (fresh->value.empty() ? "(空)" : fresh->value.c_str())
                                  : fresh.error().message.c_str());
    if (!ok) ++failures;
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("最小闭环全部通过\n");
  return 0;
}
