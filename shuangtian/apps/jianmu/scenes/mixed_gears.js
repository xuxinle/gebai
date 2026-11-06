// ============================================================================
// 混合工作流示例 · 组装端（three.js）
// ----------------------------------------------------------------------------
// 几何全部来自 `mixed_gears_asset.py`（bpy 造的齿轮）——three.js 这边一行几何
// 代码都没有，只负责**摆位、材质变体、灯光、相机、动画**。
//
// ## 这就是 bpy × three.js 的混合方式
//
// 两个脚本同时写进一份场景描述（JSON）时：
//
// ```json
// { "bpy_script": "mixed_gears_asset.py", "three_script": "mixed_gears.js" }
// ```
//
// 语义是确定的（见 `docs/scene3d_capability_plan.md` 与 `SceneSpec::bpy_script`）：
//   ① 先跑 bpy，把每个带 mesh 的物体导出成 `THREE.__assets.<物体名>`；
//   ② 再跑本脚本，它把那些几何当成自己的 `BufferGeometry` 用；
//   ③ **相机/灯光/物体全部以本脚本为准**——bpy 只供几何。
//
// 于是不存在"两套相机谁生效"的问题：three.js 是最终场景的唯一权威。
//
// ## 为什么这样分工
//
// | | bpy | three.js |
// |---|---|---|
// | 擅长 | 齿形/挖孔/布尔/倒角（bmesh 逐顶点） | 层级、材质变体、相机、动画、实例化 |
// | 本文件用它做 | 造两个齿轮 | 摆啮合位置、四档材质、布光、转动 |
//
// 手工版（`movement_showcase.js`）是"bpy 导出 JSON → 工具内联进脚本"——
// 那需要一步生成期工具。混合模式把这一步省掉了：**同一次求值里跑完两个解释器**。
// ============================================================================

const ASSET = THREE.__assets || {};
const gearA = ASSET['gear_teeth_28'];
const gearB = ASSET['gear_teeth_12'];
if (!gearA || !gearB) {
  // 明确报错而不是画个空场景：资产没到，说明 bpy 那侧没产出（或名字变了）。
  throw new Error('bpy 资产缺失：期望 gear_teeth_28 / gear_teeth_12，实际 [' +
                  Object.keys(ASSET).join(', ') + ']');
}

const TAU = 6.283185307179586;

// ---- 材质：同一个几何，四档质感（演示"资产复用"的意义）----
const brass  = new THREE.MeshStandardMaterial({ color: 0xc89a3a, metalness: 0.95, roughness: 0.22 });
const steel  = new THREE.MeshStandardMaterial({ color: 0x9aa6b8, metalness: 0.92, roughness: 0.16 });
const copper = new THREE.MeshStandardMaterial({ color: 0xb4653a, metalness: 0.55, roughness: 0.30 });
const rubber = new THREE.MeshStandardMaterial({ color: 0x2a2f38, metalness: 0.0,  roughness: 0.85 });

const scene = new THREE.Scene();
// 黄昏冷调背景：给金属一个可反射的"天"，否则一片死黑像塑料。
scene.background = new THREE.Color(0.035, 0.045, 0.062);
scene.environmentIntensity = 0.75;

const camera = new THREE.PerspectiveCamera(34, 16 / 9, 0.05, 200);
camera.position.set(0.6, -5.4, 2.9);
camera.lookAt(0.0, 0.0, 0.0);
scene.add(camera);

// ---- 布光：主光 + 冷补光（避免暗部糊成一团）----
const key = new THREE.DirectionalLight(0xfff0d8, 2.6);
key.position.set(3.2, -3.0, 4.6);
scene.add(key);
const fill = new THREE.DirectionalLight(0x8fb6ff, 0.85);
fill.position.set(-3.4, 2.6, 1.8);
scene.add(fill);
scene.add(new THREE.AmbientLight(0x33405a, 0.5));

// ---- 啮合布局：中心距 = m(z1+z2)/2 ----
//
// 主轮 28 齿、惰轮 12 齿，模数相同（脚本里两边用同一个 pitch_radius 比例），
// 所以中心距 = r1 + r2。这不是随手放两个圆——**真的啮合**是这套资产的意义。
const r1 = 1.30;
const r2 = 0.58;
const center_distance = r1 + r2;

const main = new THREE.Mesh(gearA, brass);
main.name = 'gear_main';
main.userData.spin = { axis: [0, 0, 1], speed: 0.45 };
scene.add(main);

const idler = new THREE.Mesh(gearB, steel);
idler.name = 'gear_idler';
idler.position.set(center_distance, 0, 0);
// 惰轮**反向**转：啮合传动的方向关系，速度按齿数比缩放（同一条运动链）。
idler.userData.spin = { axis: [0, 0, 1], speed: -0.45 * (28 / 12) };
scene.add(idler);

// 第三档：同一个齿轮几何、换材质（铜），摆在下方做"零件陈列"
const spare = new THREE.Mesh(gearB, copper);
spare.name = 'gear_spare_copper';
spare.position.set(center_distance * 0.5, -1.5, -0.35);
spare.rotation.z = 0.4;
scene.add(spare);

// 第四档：橡胶质感（对比"同一几何、材质决定观感"）
const bumper = new THREE.Mesh(gearB, rubber);
bumper.name = 'gear_bumper_rubber';
bumper.position.set(-1.35, -1.5, -0.35);
bumper.rotation.z = -0.25;
scene.add(bumper);

// ---- 地台：给一组悬空零件一个"落点"--（薄片，不参与取景）----
const plate = new THREE.Mesh(
  new THREE.BoxGeometry(4.4, 4.4, 0.08),
  new THREE.MeshStandardMaterial({ color: 0x161b24, metalness: 0.2, roughness: 0.9 }));
plate.name = 'plate';
plate.position.set(0, 0, -0.42);
plate.userData.noFrame = true;
scene.add(plate);

scene;
