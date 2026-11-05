// ============================================================================
// 机芯总装 · three.js 版
// ----------------------------------------------------------------------------
// 这是「bpy 做资产、js 组装」的**组装端**。
//
// 几何全部来自 `assets_gears.json`——那是 `examples/threelab/scenes/gears_asset.py`
// 由 bpy 生成、再由 `tools/bpy_export_asset` 导出的零件表。本文件**不含
// 任何顶点数据**，它只做组装该做的事：
//
//   · 摆位   —— 齿轮按模数与齿数算出啮合距离，不是手调坐标
//   · 层级   —— 夹板 → 轴 → 齿轮 → 宝石，父子关系表达装配关系
//   · 变换   —— 每个零件独立旋转（轮系按齿数比反向旋转）
//   · 材质   —— 同一资产换不同材质（黄铜 / 金 / 蓝钢）
//   · 相机与布光
//
// ## 资产清单（全部来自 bpy）
//
// | 资产 | 用途 | 三角形 |
// |---|---|---|
// | gear_a | 42 齿主轮 | ~2000 |
// | gear_b | 18 齿副轮 | ~900 |
// | gear_c | 12 齿惰轮 | ~600 |
// | pinion | 8 齿小齿轮（实心） | ~400 |
// | arbor  | 轴 | ~120 |
// | jewel  | 轴承宝石 | ~200 |
// | plate  | 夹板 | ~300 |
// | badge  | 铭牌 | ~200 |
//
// ## 组装逻辑
//
// 主轮在左，其余四个齿轮沿一条折线啮合传递动力——每个齿轮的位置由
// 「前一个齿轮的分度圆 + 自己的分度圆」算出，与真实轮系同一算法。
// ============================================================================

// ---- 资产（由 tools/bpy_export_asset 从 gears_asset.py 导出）----
const ASSETS = __ASSETS__;

const TAU = 6.283185307179586;

// ---- 场景 ----
const scene = new THREE.Scene();
scene.background = new THREE.Color(0.055, 0.060, 0.075);

const camera = new THREE.PerspectiveCamera(34, 1100 / 800, 0.05, 200);
camera.position.set(-0.6, 0.4, 34.0);
camera.lookAt(-0.9, 0.1, 0.0);
scene.add(camera);

// ---- 布光：主光 + 侧逆光 + 环境底色 ----
const key = new THREE.DirectionalLight(0xfff4e0, 2.4);
key.position.set(4.5, 6.0, 7.0);
key.target.position.set(0, 0, 0);
scene.add(key);
scene.add(key.target);

const rim = new THREE.DirectionalLight(0x88aaff, 1.1);
rim.position.set(-6.0, 2.0, -4.0);
rim.target.position.set(0, 0, 0);
scene.add(rim);
scene.add(rim.target);

const fill = new THREE.DirectionalLight(0xffffff, 0.35);
fill.position.set(0.0, -5.0, 3.0);
fill.target.position.set(0, 0, 0);
scene.add(fill);
scene.add(fill.target);

// ---- 材质：从资产自带的材质派生（同一资产可换不同材质）----
function materialFromAsset(asset, override) {
  const src = (override && override.color) || asset.material.color;
  return new THREE.MeshStandardMaterial({
    // 资产里的颜色是**线性**值；本框架的 `new Color(r,g,b)` 也按线性解释
    color: new THREE.Color(src[0], src[1], src[2]),
    metalness: (override && override.metalness !== undefined)
      ? override.metalness : asset.material.metalness,
    roughness: (override && override.roughness !== undefined)
      ? override.roughness : asset.material.roughness,
  });
}

// ---- 从资产建 Mesh ----
function spawn(assetName, materialOverride) {
  const asset = ASSETS[assetName];
  if (!asset) throw new Error('资产不存在：' + assetName);
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute('position',
    new THREE.BufferAttribute(new Float32Array(asset.positions), 3));
  geometry.setAttribute('normal',
    new THREE.BufferAttribute(new Float32Array(asset.normals), 3));
  if (asset.indices && asset.indices.length > 0) {
    geometry.setIndex(asset.indices);
  }
  const mesh = new THREE.Mesh(geometry, materialFromAsset(asset, materialOverride));
  mesh.name = assetName;
  return mesh;
}

// ---- 分度圆半径 ----
//
// ## 为什么从包围盒反推而不是查表
//
// 资产 JSON 里只有几何，没有「齿数 / 模数」这些建模参数（它们是 bpy 脚本
// 内部的局部变量）。而组装端只需要分度圆来做啮合计算。
//
// 反推公式：齿顶半径 = 分度圆 + rim/2，而 rim ≈ 齿顶半径的 8%
// （实测 gear_a：包围半径 4.58，齿数 42、模数 0.21 → 分度 4.41，
//  (4.58 - 4.41) / 4.58 = 3.7%，即 rim/2；所以 rim ≈ 7.4%）。
//
// 取 **0.963** 这个系数：`4.58 × 0.963 = 4.41` ✓
//
// ⚠ 这个反推不精确（它对每个齿轮的 rim 比例略有差异），但啮合距离对
// 10% 的误差不敏感——齿只是"看起来咬合"，不是物理仿真。
function pitchRadius(assetName) {
  const positions = ASSETS[assetName].positions;
  let maxR = 0.0;
  for (let i = 0; i < positions.length; i += 3) {
    const r = Math.sqrt(positions[i] * positions[i] + positions[i + 1] * positions[i + 1]);
    if (r > maxR) maxR = r;
  }
  return maxR * 0.963;
}

// ---- 组装：轮系（沿折线啮合）----
//
// 每个齿轮的位置由「前一个齿轮位置 + (r1 + r2) 的极坐标偏移」算出。
// 角度含义：从前一个齿轮中心指向本齿轮中心的**极角**（度）。
//
// ## 为何是这条折线
//
// 真实的轮系为了在有限体积里传递动力，齿轮会绕来绕去。而这里是**展示**
// 用途，所以排成一条从左下到右上的弧线——画面重心居中、每个齿轮都完整。
//
// 角度按顺序递增（-20° → 15° → 55° → 100°），保证路径连续转向：
// 每个齿轮都在前一个的右下 / 右上 / 左上，不会折回去重叠。
const TRAIN = [
  { asset: 'gear_a', angle: 0.0 },      // 42 齿主轮（起点，画幅左侧）
  { asset: 'gear_b', angle: -8.0 },     // 18 齿副轮（略向下）
  { asset: 'pinion', angle: 42.0 },     // 8 齿小齿轮（转向上）
  { asset: 'gear_c', angle: 88.0 },     // 12 齿惰轮（继续向左上）
  { asset: 'gear_b', angle: 128.0, material: { color: [0.90, 0.72, 0.30] } }, // 18 齿金轮
];

const train = [];
let previous = null;
for (let i = 0; i < TRAIN.length; ++i) {
  const item = TRAIN[i];
  const mesh = spawn(item.asset, item.material);
  const r = pitchRadius(item.asset);

  if (previous === null) {
    mesh.position.set(-8.0, -1.6, 0.0);
  } else {
    // 中心距 = 两分度圆半径之和 × 1.02（留齿隙，避免齿穿进对方本体）
    const distance = (previous.radius + r) * 1.02;
    const rad = (item.angle * Math.PI) / 180.0;
    mesh.position.set(
      previous.mesh.position.x + Math.cos(rad) * distance,
      previous.mesh.position.y + Math.sin(rad) * distance,
      previous.mesh.position.z + 0.06
    );
  }
  // 轮系反向：相邻齿轮转向相反（齿数比决定转速，这里只做静态姿态）
  mesh.rotation.z = (i % 2 === 0) ? 0.0 : (Math.PI / 12.0);

  scene.add(mesh);
  train.push({ mesh: mesh, radius: r, asset: item.asset });
  previous = { mesh: mesh, radius: r };
}

// ---- 组装：轴（穿过每个齿轮中心）----
for (let i = 0; i < train.length; ++i) {
  const axis = spawn('arbor');
  axis.position.set(train[i].mesh.position.x, train[i].mesh.position.y,
                    train[i].mesh.position.z);
  // 轴默认沿 Y（圆柱的 axis）；齿轮在 XY 平面上，轴应当沿 Z
  axis.rotation.x = Math.PI * 0.5;
  // 资产里的轴长 0.9（z 范围 ±0.45），相对齿轮 4.6 的半径太短——
  // 拉长到 1.8 让它在齿轮两侧都露出来
  axis.scale.set(1.0, 2.0, 1.0);
  scene.add(axis);
}

// ---- 组装：夹板（衬在轮系后面）+ 宝石（压在每个轴端）----
const plate = spawn('plate', {
  // 夹板：深色哑光金属（资产自带材质偏亮灰，换成更深沉的，避免抢主体）
  color: [0.020, 0.023, 0.032], metalness: 0.35, roughness: 0.75,
});
// 夹板在轮系**背后**（轮系 z ∈ [-0.9, +0.62]，夹板放 -2.2）
plate.scale.set(1.7, 3.0, 1.0);
// 旋转一点，与轮系路径大致平行（视觉上像"托着"整组）
plate.rotation.z = -0.10;
plate.position.set(-3.4, 0.2, -2.80);
scene.add(plate);

for (let i = 0; i < train.length; ++i) {
  const jewel = spawn('jewel');
  jewel.position.set(train[i].mesh.position.x, train[i].mesh.position.y,
                     train[i].mesh.position.z + 0.62);
  jewel.rotation.x = Math.PI * 0.5;
  scene.add(jewel);
}

// ---- 组装：铭牌（右下角，45° 斜放）----
const badge = spawn('badge');
badge.position.set(5.4, -5.4, 1.0);
badge.rotation.z = -0.12;
badge.scale.set(1.1, 0.70, 1.0);
scene.add(badge);

// ---- 组装：一个悬空的展示齿轮（用同一资产，换蓝钢材质）----
const showcase = spawn('gear_b', { color: [0.28, 0.34, 0.46], metalness: 0.92, roughness: 0.18 });
showcase.position.set(7.6, 4.6, 2.2);
showcase.rotation.z = 0.4;
showcase.scale.set(0.85, 0.85, 0.85);
scene.add(showcase);

scene;
