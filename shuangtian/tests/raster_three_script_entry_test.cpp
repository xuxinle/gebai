/// three.js 脚本的**入口分析**回归：自动给最后一条语句加 `return`。
///
/// ## 为什么这条必须单独钉住
///
/// 脚本被包进 `(function(){ ... })()` 里求值（这样才能多次求值不撞重声明），
/// 而“最后一句就是场景”这个有用语义要靠一处**源码改写**保留。旧实现只看最后一行，
/// 于是三种**完全合法**的写法都会被它改坏：
///
/// | 写法 | 旧实现产生的代码 | 现象 |
/// |---|---|---|
/// | 表达式跨行（`new THREE.Mesh(\n  geo, mat)`） | `return new THREE.Mesh(` + 换行 | 语法错 |
/// | 一行多个语句（`s.add(a);s.add(b);s`） | `return s.add(a);s.add(b);s` | 返回值不是场景 |
/// | 末行是块结尾 | 识别为空 → 不加 return | 场景丢 |
///
/// 实测成本：一组 20 条能力脚本里有 16 条因此报错或丢场景，而**错误信息指向用户
/// 完全无辜的最后一行**——最容易的误判就是“这个能力不支持”。
///
/// 所以这里的判据都是“跑一遍看能不能产出场景”，而不是检查改写出来的源码文本
/// （文本对不上不代表语义错，反之亦然）。

#include <string>

#include "st/raster/three_runtime.hpp"
#include "st/test/test.hpp"

using st::raster::ThreeRuntime;

namespace {

auto evaluate(const char* script) {
  return ThreeRuntime::instance().evaluate(script, "<test>");
}

}  // namespace

ST_TEST(three_script_multiline_last_expression_keeps_scene) {
  if (!ThreeRuntime::available()) return;
  // 最后一条表达式**跨多行**（三个参数各占一行）。旧实现会在第一行就插 return。
  const auto scene = evaluate(R"js(
const scene = new THREE.Scene();
scene.add(new THREE.Mesh(
  new THREE.BoxGeometry(1, 1, 1),
  new THREE.MeshStandardMaterial({ color: 0x88aaff })
));
scene
)js");
  ST_REQUIRE(scene.has_value());
  ST_CHECK(!scene->objects.empty());
}

ST_TEST(three_script_multiple_statements_on_last_line_returns_scene) {
  if (!ThreeRuntime::available()) return;
  // 末行有多个语句：`scene.add(x);scene.add(y);scene`。
  // 旧实现得到 `return scene.add(x);...` → 返回值是 add 的返回值（不是场景），
  // 于是靠全局名兜底也找不回（IIFE 里 `scene` 是局部 const）。
  const auto scene = evaluate(R"js(
const scene = new THREE.Scene();
const a = new THREE.Mesh(new THREE.BoxGeometry(1,1,1));
const b = new THREE.Mesh(new THREE.BoxGeometry(1,1,1));
b.position.set(2, 0, 0);
scene.add(a);scene.add(b);scene
)js");
  ST_REQUIRE(scene.has_value());
  ST_CHECK(scene->objects.size() == 2);
}

ST_TEST(three_script_trailing_block_does_not_swallow_scene) {
  if (!ThreeRuntime::available()) return;
  // 最后一条**语句**是一个 for 块，而场景根在更早的 `const scene = ...`。
  // 这是 `InstancedMesh` 一类脚本的常见形状（先建矩阵，再 add）。
  const auto scene = evaluate(R"js(
const scene = new THREE.Scene();
const geo = new THREE.BoxGeometry(0.2, 0.2, 0.2);
const mat = new THREE.MeshStandardMaterial({ color: 0xaaaaaa });
const inst = new THREE.InstancedMesh(geo, mat, 8);
const mx = new THREE.Matrix4();
let i = 0;
for (let x = -1; x <= 1; x += 2) { for (let y = -1; y <= 1; y += 2) {
  mx.makeTranslation(x * 0.5, y * 0.5, 0);
  inst.setMatrixAt(i++, mx);
} }
scene.add(inst);
scene
)js");
  ST_REQUIRE(scene.has_value());
  ST_CHECK(!scene->objects.empty());
}

ST_TEST(three_script_console_does_not_break_the_scene) {
  if (!ThreeRuntime::available()) return;
  // 沙箱里曾经没有 `console` → 一句 `console.log` 就把整份脚本打断
  // （`console is not defined`），现象是“场景出不来”而原因看起来完全无辜。
  const auto scene = evaluate(R"js(
const scene = new THREE.Scene();
console.log('building scene');
scene.add(new THREE.Mesh(new THREE.SphereGeometry(0.5, 16, 12)));
scene
)js");
  ST_REQUIRE(scene.has_value());
  ST_CHECK(!scene->objects.empty());
}

ST_TEST(three_script_explicit_return_still_wins) {
  if (!ThreeRuntime::available()) return;
  // 已经写了 `return` 的脚本不能被再包一层（`return return x` 是语法错）。
  const auto scene = evaluate(R"js(
const scene = new THREE.Scene();
scene.add(new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1)));
return scene;
)js");
  ST_REQUIRE(scene.has_value());
  ST_CHECK(!scene->objects.empty());
}
