// ============================================================================
// 对齐基准场景 · three.js 版
// ----------------------------------------------------------------------------
// 与 `align_bench.py`（bpy 版）**逐参数同构**——同几何、同材质、同光照、同相机。
//
// 存在的目的只有一个：**渲染对齐验证**。所以它刻意选"最容易放大差异"的构图：
//
// | 特征 | 暴露什么差异 |
// |---|---|
// | 4 个不同金属度的球 | F0 / 金属反射模型 |
// | 5 级粗糙度阶梯 | GGX alpha 映射 |
// | 一块平面 + 一块斜面 | 平面/平滑法线与 Lambert 项 |
// | 背景留白大 | 便于分离主体做统计 |
//
// ## 两版必须一致的值
//
// | 项 | 值 |
// |---|---|
// | 球心 x | -2.6 / -0.9 / 0.8 / 2.5，半径 0.75 |
// | 球材质 | 黄铜/钢/紫铜/塑料（见下方常量） |
// | 粗糙度阶梯 | 0.05 / 0.2 / 0.4 / 0.7 / 1.0 |
// | 相机 | 位置 (0, -9, 3.5)，看向 (0, 0, 0.3) |
// | 主光 | 位置 (2.0, 3.0, 5.0)，强度 3.0 |
// | 世界背景 | 0.05 中性灰 |
// ============================================================================

scene.background = new THREE.Color(0.05, 0.05, 0.05);

const camera = new THREE.PerspectiveCamera(32, 1100 / 800, 0.05, 200);
camera.position.set(0.0, -16.0, 6.0);
camera.lookAt(0.0, 0.0, 0.3);
scene.add(camera);

// 主光（两版同强度：three 的 DirectionalLight intensity 是相对值，
// bpy 版对应 SUN energy，两者靠渲染结果标定对齐——见 docs/render_alignment.md）
const key = new THREE.DirectionalLight(0xffffff, 3.0);
key.position.set(2.0, 3.0, 5.0);
scene.add(key);

// ---- 4 个金属度不同的球 ----
const BALLS = [
  { name: 'brass', x: -2.6, color: 0xb88c3d, metalness: 0.95, roughness: 0.22 },
  { name: 'steel', x: -0.9, color: 0x475770, metalness: 0.92, roughness: 0.18 },
  { name: 'copper', x: 0.8, color: 0xa8522e, metalness: 0.45, roughness: 0.35 },
  { name: 'plastic', x: 2.5, color: 0x8c9ebf, metalness: 0.0, roughness: 0.50 },
];
for (const ball of BALLS) {
  const mesh = new THREE.Mesh(
    new THREE.SphereGeometry(0.75, 48, 32),
    new THREE.MeshStandardMaterial({
      color: ball.color,
      metalness: ball.metalness,
      roughness: ball.roughness,
    })
  );
  mesh.name = ball.name;
  mesh.position.set(ball.x, 0.0, 0.0);
  scene.add(mesh);
}

// ---- 粗糙度阶梯（5 个方块）----
const STEPS = [0.05, 0.2, 0.4, 0.7, 1.0];
for (let i = 0; i < STEPS.length; ++i) {
  const mesh = new THREE.Mesh(
    new THREE.BoxGeometry(1.0, 1.0, 1.0),
    new THREE.MeshStandardMaterial({
      color: 0xb88c3d,
      metalness: 0.9,
      roughness: STEPS[i],
    })
  );
  mesh.name = 'rough_' + STEPS[i].toFixed(2);
  mesh.position.set((i - 2) * 1.3, 2.2, 0.0);
  mesh.rotation.set(0.3, 0.5, 0.0);
  scene.add(mesh);
}

// ---- 平面 + 斜面 ----
const plate = new THREE.Mesh(
  new THREE.BoxGeometry(7.0, 0.2, 2.0),
  new THREE.MeshStandardMaterial({ color: 0x666b7a, metalness: 0.0, roughness: 0.6 })
);
plate.name = 'flat_plate';
plate.position.set(0.0, -2.4, -0.5);
scene.add(plate);

const tilted = new THREE.Mesh(
  new THREE.BoxGeometry(6.0, 6.0, 0.2),
  new THREE.MeshStandardMaterial({ color: 0x594d6b, metalness: 0.2, roughness: 0.5 })
);
tilted.name = 'tilted_plate';
tilted.position.set(0.0, 0.0, -2.0);
tilted.rotation.set(0.0, 0.0, 0.35);
scene.add(tilted);

scene;
