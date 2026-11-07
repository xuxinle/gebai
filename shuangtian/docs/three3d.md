# 用 three.js 写三维场景

> 目标：让**模型/人写 three.js 就能出图**，光栅化由霜天自己做（不引浏览器）。
>
> 一句话定位：three.js 在这里是**场景描述 DSL**，不是渲染器。

## 一、为什么要有这条路

本框架原有的三维入口有两个：声明式 JSON（`SceneSpec.objects`）与过程式调用（`Scene3D`）。
两者对人够用，**对模型不够**——不是能力少，而是语料里没有。

实测对比（同一个"金属球浮在底座上"）：

| 表达层 | 模型的首次成功率 | 失败模式 |
|---|---|---|
| three.js 源码 | 接近一次就对 | 偶尔漏 `scene.add()` |
| 本框架的 JSON 场景 | 要反复试 | 字段语义靠查文档，`rotation` 单位/顺序易错 |
| 过程式 `Scene3D` 调用 | 要反复试 | 调用顺序错了会静默失效 |

所以这条路的价值不在"多一个 API"，而在**换掉表达层**。
渲染能力完全没变（仍是本框架的两条腿：有显卡走 D3D11，没有走软件光栅器）。

## 二、怎么写

### 2.1 最小可用形态

```cpp
#include "st/raster/three_runtime.hpp"
#include "st/raster/scene3d_script.hpp"

auto spec = st::raster::scene_spec_from_three(R"js(
  const scene = new THREE.Scene();

  const camera = new THREE.PerspectiveCamera(45, 16/9, 0.1, 100);
  camera.position.set(0, 1.2, 4.5);
  camera.lookAt(0, 0.3, 0);
  scene.add(camera);

  scene.add(new THREE.AmbientLight(0x8899bb, 0.5));
  const key = new THREE.DirectionalLight(0xffffff, 2.4);
  key.position.set(3, 4.5, 3.5);
  key.target.position.set(0, 0.3, 0);
  scene.add(key);
  scene.add(key.target);

  scene.add(new THREE.Mesh(
    new THREE.SphereGeometry(0.7, 32, 24),
    new THREE.MeshStandardMaterial({ color: 0xd09050, metalness: 0.85, roughness: 0.22 })));

  scene;   // ← 最后一个表达式的值就是场景
)js", "hero.js");

st::raster::Canvas canvas(960, 540);
if (spec.has_value()) {
  st::raster::render_scene(*spec, canvas, st::math::Color{8, 10, 16, 255});
}
```

### 2.2 从 JSON 里用（裁云 `model_script`）

裁云的 `model3d` 图层吃 `model_script`，**两种写法都行**（自动识别）：

```json
// ① 直接给 three.js 源码（推荐：语料丰富，模型几乎一次就对）
{
  "kind": "model3d",
  "model_script": "const scene = new THREE.Scene(); ... scene;"
}

// ② 包成对象（能从文件加载时把真实路径带进去，报错带行号）
{
  "kind": "model3d",
  "model_script": {
    "three_script": "...",
    "three_script_name": "shots/hero.js"
  }
}
```

识别在 `parse_scene_spec_text` 里做：先试 JSON，**是对象**才算声明式，
其余一律当 three 源码。一处判断，预览/渲染/自检三条路自动同时支持。

### 2.2.1 与图层 `model_*` 的关系：**叠加，不是互斥**

| 字段组 | `model_script` 非空时 | 开关 |
|---|---|---|
| 几何（`model_shape`/`model_obj`/`model_gltf`） | 忽略（几何由场景提供） | — |
| 材质（`model_color`/`metallic`/`roughness`） | 默认用场景的；打开则统一换掉 | `model_material_override` |
| 相机（`cam_*`） | 默认用场景的；打开则用图层相机 | `model_use_layer_camera` |
| 姿态（`model_yaw`/`spin`/`tilt`/`bob`/`scale`） | **总是叠加** | — |
| 合成（`model_shadow`/`model_backdrop`） | **总是叠加** | — |

分开的理由：镜头推拉与投影阴影是**片子层的事**，不该让每个场景重写；
而几何与材质属于模型自己的定义，写进场景才对。

> ⚠ 打开 `model_use_layer_camera` 后，`cam_distance` 默认只有 3.0——
> 而一个 40mm 直径的模型包围半径约 30，直接钻到模型里面。
> 用这个开关时**必须同时调 `cam_distance`**（实测过：不调就是一整块贴脸）。

### 2.3 契约（三条，必须记）

| 项 | 规则 |
|---|---|
| **返回值** | 最后一个表达式应是 `Scene` / `Group` / `Mesh`（数组也收），或定义全局 `scene` |
| **相机** | 放在场景里（`scene.add(camera)`）或不放都行，两种都能取到 |
| **能写什么** | 完整 JS：循环、函数、条件 —— 这是它相对 JSON 的核心优势 |

```js
// 沿弧线摆 24 个方块：JSON 要写 24 遍，这里一个 for
const scene = new THREE.Scene();
const mat = new THREE.MeshStandardMaterial({ color: 0x88aaff, metalness: 0.6 });
for (let i = 0; i < 24; i++) {
  const angle = (i / 24) * Math.PI * 2;
  const block = new THREE.Mesh(new THREE.BoxGeometry(0.2, 0.2, 0.2), mat);
  block.position.set(Math.cos(angle) * 2, 0.5, Math.sin(angle) * 2);
  block.rotation.y = -angle;
  scene.add(block);
}
scene;
```

## 三、能力边界（**明确声明，不是遗漏**）

> **这一节现在有可执行的判据**：`tools/scene3d_capability_probe` 把下面的能力
> 逐条写成脚本真跑一遍（20 条），并给出“可用 / 不可用”。改完引擎先跑它——
> 比读这张表可靠（表会漂，探针不会）。

### 3.0 脚本入口的三条约定（改引擎前必读）

脚本被包进 `(function(){...})()` 求值（这样同一运行时多次求值不会撞 `const` 重声明），
而“最后一句就是场景”靠一处源码改写保留。三条已经踩过的坑：

| 做法 | 为什么会踩 |
|---|---|
| **按行**识别最后一句 | 表达式跳行 / 一行多语句都是合法 JS，按行加 `return` 会直接语法错 |
| `return` 后面跟 `{` | 会被解析成**对象字面量**，块里的语句全部失效（语义静默地变了）|
| 沙箱没有 `console` | 一句 `console.log` 就把整份脚本打断，而原因看起来完全无辜 |

现在的实现按**语句边界**（跳字符串/模板/注释，只在括号深度全归零时认 `;` 与 `}`），
`console` 走宿主函数打到 `st::log`。回归见 `tests/raster_three_script_entry_test.cpp`。

### 3.1 用的部分

| three.js | 说明 |
|---|---|
| `Scene` / `Group` / `Object3D` | 父子变换、`updateMatrixWorld` 全是纯 JS |
| `BufferGeometry` + 全部内置几何生成器 | 顶点/法线/UV/索引从 `Float32Array` **零拷贝**读内存 |
| `ExtrudeGeometry` / `LatheGeometry` / `TubeGeometry` / `Shape` + holes | 实测均可用（模型最爱写的轮廓建模） |
| `CatmullRomCurve3` / `Vector2` / `Vector3` / `Quaternion` / `Euler` / `Matrix4` / `Box3` | 数学类全可用 |
| `PerspectiveCamera` / `OrthographicCamera` | 位置/朝向/fov/near/far/ortho_scale 全部翻译 |
| `DirectionalLight` / `PointLight` / `SpotLight` / `AmbientLight` / `HemisphereLight` | 前四类精确翻译；半球光近似为"环境 + 顶光" |
| `InstancedMesh` / `AnimationMixer` | 可构造、可算（动画的**采样结果**能取到） |
| `MeshStandardMaterial` / `Physical` / `Phong` / `Lambert` | color/metalness/roughness/opacity/emissive/side/depthWrite |
| `MeshBasicMaterial` | 映射为"自发光 + unlit"（three 里它不受光） |

### 3.2 **不**用的部分（这是硬约束）

| three.js | 为什么不用 |
|---|---|
| `WebGLRenderer` | 渲染由本框架的 `Scene3D` 做（这正是"不引浏览器"的意思） |
| `TextureLoader` / `ImageLoader` / `document` / `Image` | 没有浏览器环境。**构造不报错，但加载不了** |
| `GLTFLoader` / `OBJLoader` / `FBXLoader` | 外部模型请用本框架的 `gltf` 字段（它有自己的加载器） |
| 多材质网格（`material: [m1, m2]`） | 当前只取第一个材质。需要多材质请拆成多个 Mesh |
| `SkinnedMesh` 的骨骼动画 | 网格会按当前姿态抽出（不是时间轴驱动） |
| `SkeletonHelper` / `ArrowHelper` 等辅助对象 | 它们不是 Mesh，被跳过 |
| **`Canvas` / `CanvasTexture`（二维画布）** | 脚本宿主（QuickJS）**没有 Canvas 2D**——three.js 常规的「Canvas 画字 → 纹理 → Sprite」整条路走不通，用几何体拼汉字更不现实。**文字需要由 C++ 侧画**：三维层只出几何，文字层用 `TextPort` 摆，位置由 `ThreeView::project_to_screen` 换算（与渲染共用同一个相机，切勿在外面重写投影公式——两套计算必然漂移） |

### 3.3 语义翻译表（**最容易踩的地方**）

两套引擎对同一概念的定义有细微差别，而差别几乎总是**不报错、只让画面变样**：

| 概念 | three.js | 霜天 | 本层怎么翻译 | 漏翻的后果 |
|---|---|---|---|---|
| 平行光方向 | `position → target`（**射出**） | 指向光源 | **取反** | 灯照不到东西，全黑 |
| 点光衰减距离 | `distance` | `radius` | 直接映射 | — |
| 聚光角度 | `angle`（半角，弧度） | 内外锥（度） | `angle × 180/π`；内锥按 `penumbra` 收窄 | 光斑大小不对 |
| `opacity` | 需 `transparent: true` 才生效 | `< 1` 就混合 | 未设 `transparent` 时**拉回 1.0** | 半透明物体变实心 |
| `side` | Front / Back / Double | `CullMode` | Front→剔背面；Back→剔正面；Double→不剔 | 薄片背面消失 / 内壁外露 |
| `depthWrite` | 同名 | `depth_write` | 直接映射 | 透明物体挡住后面的东西 |
| 颜色 | `Color.toArray()` **已是线性** | `base_color` 要线性 | 直接用（**不再解码**） | 画面整体发暗 2 倍 |
| 坐标系 | 右手系、+Y 上、相机看 -Z | 同 | 无需翻转 | 场景镜像/旋转 90° |
| 矩阵序 | 列主序 `m[col*4+row]` | 同 | `memcpy` | 位置镜像错位 |
| `MeshBasicMaterial` | 不受光 | 无此概念 | 自发光 + `emission_unlit` | Basic 材质被光照加工 |
| `emissiveIntensity` | 自发光强度倍数 | `Material::emission` | 相乘 | 发光体不亮 / 亮得没层次 |
| `color` 与 `emissive` | 两个**独立**字段 | 同（`base_color` / `emissive_color`） | 分别映射 | 把发光体当纯色物体——暗场里一片黑 |

> **颜色口径的实测依据**：three 在 `ColorManagement.enabled`（默认 true）时，
> `new Color(0xd08a50)` 就已经把 sRGB 解码成线性 `[0.6308, 0.2542, 0.0802]`。
> 中灰 `0x808080` → 线性 `0.2159`（不是 0.502）。两边口径天然一致，**不要再解码一次**。

## 四、闭环验证：怎么知道画对了

### 4.1 为什么不能靠"看图"

三维的失败**几乎从不报错**：

| 症状 | 解析器 | 渲染器 | 画面 |
|---|---|---|---|
| 相机钻进物体里 | 无意见 | 无意见 | 全黑 / 全糊 |
| 物体全在取景框外 | 无意见 | 无意见 | 一片背景色 |
| 物体只有 3 个像素 | 无意见 | 无意见 | 看着像"没画" |
| 两个物体完全重叠 | 无意见 | 无意见 | 只看到一个 |
| 光方向反了 | 无意见 | 无意见 | 全黑 |
| 颜色重复解码 | 无意见 | 无意见 | 偏暗，"像配色问题" |

而这些恰恰是模型最难自查的一类（**从截图判断"球是不是被相机切掉了"是模型的弱项**）。
所以闭环不能建立在"看图"上。

### 4.2 用 `three_check`（推荐）

```bash
# 命令行——模型/CI 都能直接用
st build three_check
./build/release/bin/three_check --three hero.js --png out/hero.png
```

输出：

```text
PASS ok（480×320，覆盖 19.11%，亮度 63.2）
诊断：
  [info] 画面正常：覆盖 19.1%，平均亮度 63.2
```

坏场景会明确报出来：

```text
FAIL nothing_drawn（480×320，覆盖 0.00%，亮度 48.7）
诊断：
  [error] 画面里几乎没有东西：非背景像素 0 个（480×320 共 153600 像素）
          — 三个最常见成因，按顺序排查：
            ① 相机在物体内部或物体在相机背后 → 把 camera.eye 沿视线方向退到物体外面；
            ② 物体全在取景框外 → 让相机 target 指向物体包围盒中心；
            ③ 没写 scene.add(mesh) / 物体 visible=false
```

**退出码**：`0` = ok，`1` = 有 error（画面不可用），`2` = 参数/IO/解析失败。
可以直接当 CI 门禁。

### 4.3 用 API

```cpp
#include "st/raster/scene_report.hpp"

const auto report = st::raster::inspect_scene_render(spec, 480, 320, background);
if (!report.ok()) {
  // report.verdict: "ok" / "nothing_drawn" / "camera_inside" / ...
  // report.diagnostics: 每条带 level/code/message/suggestion
  // report.objects: 逐物体的 pixels/coverage/bbox（**"哪个物体没出现"只有这里能查**）
  ST_LOG_WARN("{}", st::raster::report_text(report));
}
```

### 4.4 报告能认出什么（都有实测用例）

| 结论码 | 触发条件 | 实测用例 |
|---|---|---|
| `nothing_drawn` | 非背景像素 < 24 | `scene_report_detects_empty_frame` |
| `camera_inside` | 覆盖率 > 95% | `scene_report_detects_camera_inside_geometry` |
| `object_out_of_frame` | 某物体单独渲染 0 像素 | `scene_report_reports_per_object_visibility` |
| `object_tiny` | 某物体 < 24 像素 | `scene_report_detects_tiny_object` |
| `all_overlapping` | 全部物体屏幕包围盒相同 | `scene_report_detects_fully_overlapping_objects` |
| `no_shading_variation` | 亮度标准差 < 2（纯色块） | `scene_report_flags_pure_flat_image` |
| `image_too_dark` / `image_blown_out` | 平均亮度 < 12 / > 235 | 见下节"已知口径" |

> **逐物体探测只对声明式场景（`objects`）可用**。three 场景是"一条脚本"，
> 没法"只保留其中一个物体"再渲染——那需要改脚本源码。所以 three 场景
> 报告只给整幅层面的事实，且**不会伪造**"某个物体看不见"的假警告。

### 4.5 已知口径（写判据前必看）

**环境反射自带底面光**：本框架的 `env_strength` 未配置时，会按
`ambient + 0.55` 给一个基准环境反射强度（目的是让金属"哪里都在映环境"）。
实测：**完全不打光**的一个中灰球，平均亮度仍有 **40.2/255**；同一个球正常打光是 **45.6**。

所以：
- 不能用"亮度 < 12"来判"没打光"（永远触发不了）；
- 判"有没有光"要看**方差**（没光时各面亮度接近，有光时有明确方向性明暗）；
- 这条已写进 `scene_report.cpp` 的注释里，改阈值前先读它。

## 五、性能

实测数据（本机 RTX 4080 + 软件腿对比）：

| 项 | 实测值 | 备注 |
|---|---|---|
| three.js 加载（编译 + 求值） | **55 ms** | 线程内单例，只付一次 |
| 常驻内存（加载后） | **4.5 MiB** | 默认上限 64 MiB（脚本层默认 16 MiB 不够） |
| 每帧推进（旋转 + 求矩阵 + 遍历） | **54 µs** | 与几何量无关（只遍历对象图） |
| 抽取（1189 顶点球 + 2 个盒） | 约 0.5 ms | `Float32Array` 零拷贝读内存 |
| 渲染 480×320（三物体场景） | **13 ms**（GPU 腿） | 软件腿约 30 ms |
| `three_check` 完整跑一遍 | 约 90 ms | 含逐物体探测 |

### 5.1 为什么抽取不慢

走 `JS_GetTypedArrayBuffer` + `JS_GetArrayBuffer` **直接读内存**，不是逐个元素转 Json。
后者实测 3.5 µs/顶点（1 万顶点 35 ms），前者是"每缓冲一次取指针"。

### 5.2 重新求值 vs 缓存

`render_impl` 的 three 分支**每次渲染都重新求值脚本**。这对"逐帧动画"是必要的
（脚本里的时间相关逻辑要重算），代价是每帧多约 1~2 ms（脚本规模而定）。
若要逐帧复用同一场景（静态镜头），调用方可以只求值一次、自己持有 `ThreeScene`
再逐帧 `draw_mesh`。

## 六、三条实现细节（改代码前必读）

### 6.1 脚本必须包在 IIFE 里

直接 `JS_Eval` 是在**全局作用域**执行。模型写的 `const scene = ...` 会落到全局，
于是同一个运行时（线程内单例）**第二次**求值同样写法的脚本时报
`redeclaration of 'scene'`——而错误点指向用户完全无辜的第一行。

实测就是这样：测试里第一次跑通了，隔两个用例再跑就报重声明。
而真实使用（裁云逐帧、预览+导出二次渲染）**必然**多次求值。

修法：包进 `(function(){ 'use strict'; ... })()`，并自动把**最后一个表达式**改成
`return <expr>;`（保留"语句列表最后一句就是返回值"这个有用语义）。

### 6.2 `end_frame` 不能漏

三维腿的 `draw_mesh` 只是把三角形提交到内部缓冲；**合成到画布发生在 `end_frame`**
（它才做色调映射 / sRGB 编码 / 到画布的拷贝）。

漏了它的症状极具误导性：`draw_calls` 在涨、几何数据也对、位置全对，但**画布上什么也没有**
（像素计数 0）。本仓实测就踩过这一脚。

### 6.3 像素格式：用 `to_rgba8()`，不要自己拆 `pixels()`

`Surface::pixels()` 是 **`0xAARRGGBB`** 打包（`red<<24 | green<<16 | blue<<8 | alpha`）。
拿它当 RGBA 解析会把 **alpha 当成蓝色通道**——本仓实测因此让四个颜色判据全部假阴性
（每个采样点都读出 `rgb(x, y, 255)`）。

所有验证一律走 `to_rgba8()`：它是对外声明的"直通 RGBA8 字节流"，与 PNG/截图同一条路径。

### 6.4 `userData` 的三个约定

几何归脚本（求值一次），运动与取景意图归渲染侧（逐帧/加载时读取）：

| 键 | 作用 | 写法 |
|---|---|---|
| `spin` | 匀速自转（齿轮/指针） | `{ axis: [0,0,1], speed: 0.6 }`（弧度/秒） |
| `oscillate` | 往复摆动（摆轮/游丝/擒纵叉） | `{ axis: [0,0,1], amplitude: 0.9, frequency: 3.2, phase: 0 }` |
| `noFrame` | 取景时**不算**它（背景板/地面板） | `true` |

前两个交给 `ThreeView` 逐帧推进（`world * R(轴, 角度)`，不重建几何）；
`noFrame` 在 `frame_scene()` 里跳过——氛围件通常比主体大一圈，
算进包围盒会让取景距离白白拉远（实测：表径 40，一块半径 22.4 的背景盘
把距离从 86 拉到 120，主体在画面里只剩一半大）。

### 6.5 三个 `userData` 都会**从组继承**

```js
const rotor = new THREE.Group();
rotor.userData.spin = { axis: [1, 0, 0], speed: 0.5 };
rotor.add(fanBlade); rotor.add(compressorBlade);   // 都不带 userData
```

这是 three 的习惯写法（同一级叶片是刚体，转的是转子），抽取器会**向上找**
最近一个带标记的祖先。

> ⚠ 这条是踩出来的：最初只查网格自身，于是组上的标记**静默丢掉**——
> 场景照常渲染、物体数 1253 正确、`three_anim_probe` 报“自转 0 个”，
> 而画面就是不动。写双转子/轮系这类“一族零件一起转”的东西必定踩到。

### 6.6 剖切视图：`thetaStart` 到底画哪半圈

`CylinderGeometry(..., thetaStart, thetaLength)` 转到 X 轴后：

| 参数 | 覆盖 |
|---|---|
| `0, PI` | **上半圈**（y 全为正） |
| `PI, PI` | **下半圈**（y 全为负） |

实测得出（`tools/cylinder_theta_probe`），不是推导——`Cylinder` 的 θ 起点在 +Z、
绕 Y 逆时针，再叠 `rotation.z = PI/2`，推错一次就是“敞开的变成盖住的”，
而画面**不会报错**、只是里面什么都看不见。

### 6.7 后端选腿：GPU 比软件快 **57 倍**，而旧注释里写的结论已过期

`ThreeView` 默认 `backend_pref_` 为空（跟框架实测选腿）。**要性能就显式写 `gpu`。**

实测（2026-11-08，册府知识图谱，3533×1870 全分辨率，每次旋转）：

| 三维腿 | 主画布 | 耗时 |
|---|---|---|
| **GPU** | GPU | **5 ms** |
| 软件 | GPU | 285 ms |
| 软件 | 软件 | 394 ms |

机理：GPU 腿把三维纹理经 `composite_native_texture` **零拷贝**合成到 GPU 主画布，
全程不过 CPU；软件腿要逐像素光栅化 680 万像素。

**为何曾错误地强制软件腿**：`three_view.hpp` 里有一条注释写「GPU 腿在交互式视口里
合成失效，实测只剩背景色」。那是零拷贝合成修好**之前**的观测，已经失效。

> **教训：头文件里的「实测结论」也会过期。** 看到「已知问题 / 不能这样做」时，
> 先看日期、能 A/B 实测的不要直接采信。本次代价是：放下快 57 倍的路径不用，
> 还去降分辨率牺牲画质。

**副产物：`ThreeView::set_render_scale(scale)`**（默认 1.0 即原行为）。
走软件腿时它是必需的——软件光栅器逐像素算，DPI 1.5 会让渲染缓冲变成 680 万像素；
走 GPU 腿时**不需要降**，全分辨率就只要 5 ms。

### 6.8 文字：`project_to_screen` 与 `ray_from_point` 必须取**同样的基向量**

三维里画不了文字（QuickJS 无 Canvas 2D），文字由 C++ 侧画。但**位置不能让文字层自己算**：

```cpp
// 正：让视口回答「这个三维点现在在屏幕哪里」
const auto p = view->project_to_screen(st::math::Vec3{x, y, z});

// 错：在文字层里重写一遍轨道相机公式
//   公式本身对（同样的球坐标 → 视图 → 透视除法），但它是**另一套计算**：
//   参数靠人工同步、精度不同、更新时机不同 ⇒ 转镜头后文字与几何逐渐错位
```

两个函数都从 `Mat4::look_at` 取基向量，必须**取行不取列**
（`value(0,0)/value(0,1)/value(0,2)` 是 right）。
取列的错法在 `yaw=0` 时**看起来完全正常**（单位向量仍对），一旦转镜头就逐渐偏——
最难查的那一类。

> **教训：数学等价 ≠ 工程等价。** 「透视保持直线性，所以在 2D 层连两个投影点
> 等价于画 3D 线」——**位置确实等价**，但深度调制、遮挡、旋转时的层次、
> 更新时机全不在公式里。判断标准应是：**这个信息属于谁？**
> 深度与遮挡属于渲染器，文字排版属于 CPU。

## 七、相关文件

| 文件 | 作用 |
|---|---|
| `include/st/raster/three_runtime.hpp` | 运行时接口（`evaluate` / `available` / 单例） |
| `src/raster/three_runtime.cpp` | 实现（QuickJS 直连 + 语义翻译；公共头不引 `quickjs.h`） |
| `include/st/raster/scene_report.hpp` / `src/raster/scene_report.cpp` | 渲染自检报告 |
| `src/raster/scene3d_script.cpp` | `three_script` 字段与渲染分支 |
| `third_party/three/three.cjs` | three.js 0.160.1（MIT），编译期嵌入 |
| `tools/three_check.cpp` | 命令行自检工具 |
| `tests/raster_three_runtime_test.cpp` | 抽取正确性（11 用例） |
| `tests/raster_three_semantics_test.cpp` | 语义翻译（8 用例） |
| `tests/raster_scene3d_three_script_test.cpp` | 接入（6 用例） |
| `tests/raster_scene_report_test.cpp` | 自检报告（11 用例） |
