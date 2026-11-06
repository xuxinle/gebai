// ============================================================================
// 2×2 对比场景 · three.js 版
// ----------------------------------------------------------------------------
// 与 `quad.py`（bpy 版）逐参数同构。
//
// ## 为何用立方体而不是球
//
// 本框架 bpy 兼容层的 `primitive_uv_sphere_add` 有几何缺陷
// （顶点 x 超出半径两倍，球被压扁成椭圆——见 `docs/python.md` 的已知边界）。
// 而这份场景的目的是**对比渲染**，不是对比几何生成器。
//
// 所以两边都用**立方体**——它在两版里都是精确的（three 的 BoxGeometry
// 与 bpy 的 primitive_cube_add 都正确）。
//
// ## 场景内容
//
//   前排 4 个：金属度 0.95 / 0.92 / 0.45 / 0.0（黄铜 / 钢 / 紫铜 / 塑料）
//   后排 5 个：粗糙度 0.05 / 0.2 / 0.4 / 0.7 / 1.0（金属）
//
// 两个变化轴（金属度、粗糙度）——任何着色模型的偏差都会在这里显形。
//
// ## 两版必须一致
//
// | 项 | 值 |
// |---|---|
// | 前排 | 边长 1.4，x = -2.7/-0.9/0.9/2.7，y = -0.9，rot (0.42, 0.62, 0) |
// | 后排 | 边长 1.2，x = (i-2)×1.5，y = 1.4，rot (0.30, 0.45, 0) |
// | 相机 | (0, -12.5, 4.2) → lookAt (0, 0.2, 0)，fov 32° |
// | 主光 | 方向光，位置 (3, 4, 6)，强度 2.6 |
// | 环境 | 背景 0.05 中性灰 |
// ============================================================================

const scene = new THREE.Scene();
scene.background = new THREE.Color(0.05, 0.05, 0.05);

const camera = new THREE.PerspectiveCamera(32, 1100 / 800, 0.05, 200);
camera.position.set(0.0, -12.5, 4.2);
camera.lookAt(0.0, 0.2, 0.0);
scene.add(camera);

const key = new THREE.DirectionalLight(0xffffff, 2.6);
key.position.set(3.0, 4.0, 6.0);
key.target.position.set(0, 0.2, 0);
scene.add(key);
scene.add(key.target);

// ---- 前排：4 个金属度 ----
const METALS = [
  { name: 'brass',   x: -2.7, color: 0xb88c3d, metalness: 0.95, roughness: 0.22 },
  { name: 'steel',   x: -0.9, color: 0x475770, metalness: 0.92, roughness: 0.18 },
  { name: 'copper',  x:  0.9, color: 0xa8522e, metalness: 0.45, roughness: 0.35 },
  { name: 'plastic', x:  2.7, color: 0x8c9ebf, metalness: 0.00, roughness: 0.50 },
];
for (const m of METALS) {
  const mesh = new THREE.Mesh(
    new THREE.BoxGeometry(1.4, 1.4, 1.4),
    new THREE.MeshStandardMaterial({ color: m.color, metalness: m.metalness, roughness: m.roughness })
  );
  mesh.name = m.name;
  mesh.position.set(m.x, -0.9, 0.0);
  mesh.rotation.set(0.42, 0.62, 0.0);
  scene.add(mesh);
}

// ---- 后排：粗糙度阶梯 ----
const STEPS = [0.05, 0.2, 0.4, 0.7, 1.0];
for (let i = 0; i < STEPS.length; ++i) {
  const mesh = new THREE.Mesh(
    new THREE.BoxGeometry(1.2, 1.2, 1.2),
    new THREE.MeshStandardMaterial({ color: 0xb88c3d, metalness: 0.9, roughness: STEPS[i] })
  );
  mesh.name = 'rough_' + STEPS[i].toFixed(2);
  mesh.position.set((i - 2) * 1.5, 1.4, 0.0);
  mesh.rotation.set(0.30, 0.45, 0.0);
  scene.add(mesh);
}

globalThis.renderer = { toneMapping: 0, toneMappingExposure: 1.0, compat: 'three' };
scene;


