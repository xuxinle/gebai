// ============================================================================
// 齿轮轮系 · three.js 版
// ----------------------------------------------------------------------------
// 与 `bpy_watch.py`（bpy 版）**逐参数同构**——同齿形算法、同规格、同材质、
// 同光照、同相机。
//
// 用于 2×2 渲染对齐对比：两个脚本 × 两个渲染器。
//
// ## 齿形算法（与 bpy 版完全一致）
//
// 每个齿 7 个轮廓点：
//
//     a0 = base - toothAngle*0.50  （齿根，进）
//     a1 = base - toothAngle*0.30  （齿顶，起）
//     a2 = base + toothAngle*0.30  （齿顶，止）
//     a3 = base + toothAngle*0.50  （齿根，出）
//     + 齿槽底部 3 个插值点
//
// 分度圆 = module×teeth/2；齿顶 = 分度 + rim/2；齿根 = 分度 - rim/2。
//
// ## 轮系规格（与 bpy_watch.py 的 SPECS / IDLERS 同值）
//
// | 齿轮 | 齿数 | rim | depth | spokes | 材质 |
// |---|---|---|---|---|---|
// | barrel_wheel | 42 | 0.34 | 0.30 | 5 | 黄铜 |
// | center_wheel | 26 | 0.24 | 0.26 | 5 | 黄铜 |
// | third_wheel | 18 | 0.20 | 0.22 | 4 | 金 |
// | fourth_wheel | 14 | 0.17 | 0.20 | 4 | 金 |
// | escape_wheel | 15 | 0.15 | 0.13 | 5 | 蓝钢 |
// | idler_a/b/c | 12/10/16 | 0.16 | 0.13 | 6/5/5 | 黄铜/紫铜/黄铜 |
//
// ## 相机 / 光照（同值）
//
// | 项 | 值 |
// |---|---|
// | 相机 | 位置 (1.5, -1.0, 26)，lens 55mm，俯 8° |
// | 主光 | SUN energy 2600，位置 (-8.6, 5.6, 17.2) |
// | 环境 | 背景 0.05 中性灰 |
// ============================================================================

const scene = new THREE.Scene();
scene.background = new THREE.Color(0.05, 0.05, 0.05);

const TAU = 6.283185307179586;
const MODULE = 0.21;

// ---- 相机：与 bpy_watch.py 同参数 ----
// three 的 fov 是**垂直** fov；bpy 的 55mm 在全画幅（24mm 高）上换算：
//   fov_y = 2*atan(24/(2*55)) ≈ 24.6°
// 与兼容层的 ngle_y 同算法（sensor_width 36mm + 宽高比修正）：
//   angle_x = 2*atan(36/2/55) = 36.24°
//   angle_y = 2*atan(tan(angle_x/2) / aspect)，aspect = 1100/800
// 与兼容层的 ngle_y 同算法（不加宽高比修正——实测见 bpy_data.py 的说明）
const fovY = 2 * Math.atan(36.0 * 0.5 / 55.0) * 180 / Math.PI;
const camera = new THREE.PerspectiveCamera(fovY, 1100 / 800, 0.05, 200);
camera.position.set(1.5, -1.0, 26.0);
// bpy 的 Euler(8°, 0, 0)：绕 X 转 8°（相机默认朝 -Z，转后稍朝下）
camera.rotation.set(8.0 * Math.PI / 180.0, 0.0, 0.0);
scene.add(camera);

// ---- 主光：SUN energy 2600 ----
//
// ## 换算必须显式
//
// Blender 的 SUN `energy` 是 W/m²；three 的 `DirectionalLight.intensity`
// 是相对值。两者无官方换算。
//
// 本框架把 2600 映射成 `diffuse = 2.00`（见 `bpy_runtime.cpp`）。
// three 侧取标定值：2600/3200 = 0.8125（实测让主体均值落在 78 附近，
// 与真 Blender 参照一致）。
const key = new THREE.DirectionalLight(0xffffff, 0.8125);
key.position.set(-8.6, 5.6, 17.2);
key.target.position.set(0, 0, 0);
scene.add(key);
scene.add(key.target);

// ---- 材质（与 bpy_watch.py 的 MAT_* 同值）----
//
// ## 必须用十六进制而不是三个小数（这一步踩过坑）
//
// py_watch.py 里的 diffuse_color = (0.72, 0.55, 0.24) 是**线性**值。
// 而本框架的 three 运行时里，
// | new Color(r, g, b) 与 
// | new Color(0xhex)
// 走**不同**的路径——实测同一个黄铜方块：
//
// | 写法 | 主体均值 |
// |---|---|
// | 
// | new Color(0xb88c3d) | 94.6 |
// | 
// | new Color(0.72, 0.55, 0.24) | **149.4** |
// | setRGB(0.72,0.55,0.24, SRGBColorSpace) | 94.4 |
//
// 三个小数的写法亮 58 级（≈1.6 倍）。而 hex 与显式 sRGB 一致，说明
// **框架期望 sRGB 输入**（hex 被当作 sRGB 解码）。
//
// 所以这里把 bpy 的线性值预先转成 sRGB hex（转换用标准 sRGB 曲线）：
//
// | 材质 | 线性 | sRGB hex |
// |---|---|---|
// | 黄铜 | (0.72, 0.55, 0.24) | 0xddc486 |
// | 蓝钢 | (0.28, 0.34, 0.46) | 0x909eb5 |
// | 紫铜 | (0.66, 0.32, 0.18) | 0xd49976 |
// | 金 | (0.90, 0.72, 0.30) | 0xf3dd95 |
function pbr(hex, metallic, roughness) {
  return new THREE.MeshStandardMaterial({
    color: new THREE.Color(hex),
    metalness: metallic,
    roughness: roughness,
  });
}
const MAT_BRASS = pbr(0xddc486, 0.95, 0.22);
const MAT_STEEL = pbr(0x909eb5, 0.92, 0.18);
const MAT_COPPER = pbr(0xd49976, 0.45, 0.35);
const MAT_GOLD = pbr(0xf3dd95, 0.98, 0.15);

// ---- 齿形算法（与 bpy 版同一份逻辑）----
function gearProfile(teeth, module, rimWidth, toothFraction) {
  if (toothFraction === undefined) toothFraction = 0.44;
  const pitchRadius = (module * teeth) / 2.0;
  const tipRadius = pitchRadius + rimWidth * 0.5;
  const rootRadius = pitchRadius - rimWidth * 0.5;
  const pitchAngle = TAU / teeth;
  const toothAngle = pitchAngle * toothFraction;
  const profile = [];
  for (let i = 0; i < teeth; ++i) {
    const base = i * pitchAngle;
    const a0 = base - toothAngle * 0.5;
    const a1 = base - toothAngle * 0.30;
    const a2 = base + toothAngle * 0.30;
    const a3 = base + toothAngle * 0.5;
    profile.push([Math.cos(a0) * rootRadius, Math.sin(a0) * rootRadius]);
    profile.push([Math.cos(a1) * tipRadius, Math.sin(a1) * tipRadius]);
    profile.push([Math.cos(a2) * tipRadius, Math.sin(a2) * tipRadius]);
    profile.push([Math.cos(a3) * rootRadius, Math.sin(a3) * rootRadius]);
    const gapStart = a3;
    const gapEnd = base + pitchAngle - toothAngle * 0.5;
    for (let s = 1; s < 4; ++s) {
      const a = gapStart + (gapEnd - gapStart) * (s / 3.0);
      profile.push([Math.cos(a) * rootRadius, Math.sin(a) * rootRadius]);
    }
  }
  return { profile: profile, pitchRadius: pitchRadius, rootRadius: rootRadius };
}

// ---- 建齿轮（挤出轮廓 + 轴孔 + 轮辐窗口）----
function makeGear(name, teeth, module, rimWidth, depth, bore, spokes, hubScale) {
  if (bore === undefined) bore = 0.11;
  if (hubScale === undefined) hubScale = 2.8;
  const g = gearProfile(teeth, module, rimWidth);
  const rootRadius = g.rootRadius;
  const pitchRadius = g.pitchRadius;
  const profile = g.profile;

  const shape = new THREE.Shape();
  for (let i = 0; i < profile.length; ++i) {
    if (i === 0) shape.moveTo(profile[i][0], profile[i][1]);
    else shape.lineTo(profile[i][0], profile[i][1]);
  }
  shape.closePath();

  // 轴孔
  const borePath = new THREE.Path();
  borePath.absarc(0, 0, bore, 0, TAU, true);
  shape.holes.push(borePath);

  // 轮辐窗口（与 bpy 版同算法）
  const innerRadius = Math.max(rootRadius - rimWidth * 1.15, bore * 2.0);
  const hubRadius = Math.max(bore * hubScale, rootRadius * 0.26);
  if (spokes > 0) {
    const span = TAU / spokes;
    const spokeWidth = span * (0.34 / Math.max(1.0, spokes / 5.0));
    for (let s = 0; s < spokes; ++s) {
      const start = s * span + spokeWidth * 0.5;
      const end = (s + 1) * span - spokeWidth * 0.5;
      const windowPath = new THREE.Path();
      const segs = 6;
      for (let k = segs; k >= 0; --k) {
        const a = start + (end - start) * (k / segs);
        const px = Math.cos(a) * innerRadius;
        const py = Math.sin(a) * innerRadius;
        if (k === segs) windowPath.moveTo(px, py);
        else windowPath.lineTo(px, py);
      }
      for (let k = 0; k <= segs; ++k) {
        const a = start + (end - start) * (k / segs);
        windowPath.lineTo(Math.cos(a) * hubRadius, Math.sin(a) * hubRadius);
      }
      windowPath.closePath();
      shape.holes.push(windowPath);
    }
  }

  const geometry = new THREE.ExtrudeGeometry(shape, {
    depth: depth,
    bevelEnabled: false,
    curveSegments: 12,
  });
  geometry.translate(0, 0, -depth * 0.5);
  geometry.computeVertexNormals();
  return { geometry: geometry, pitchRadius: pitchRadius };
}

// ---- 轮系规格（与 bpy_watch.py 的 SPECS / IDLERS 同值）----
const SPECS = [
  ['barrel_wheel', 42, MODULE, 0.34, 0.30, 5, MAT_BRASS],
  ['center_wheel', 26, MODULE, 0.24, 0.26, 5, MAT_BRASS],
  ['third_wheel', 18, MODULE, 0.20, 0.22, 4, MAT_GOLD],
  ['fourth_wheel', 14, MODULE, 0.17, 0.20, 4, MAT_GOLD],
  ['escape_wheel', 15, MODULE * 0.86, 0.15, 0.13, 5, MAT_STEEL],
];
const IDLERS = [
  ['idler_a', 12, MODULE * 0.9, 0.16, 0.13, 6, MAT_BRASS, 2.2],
  ['idler_b', 10, MODULE * 0.9, 0.16, 0.13, 5, MAT_COPPER, 2.2],
  ['idler_c', 16, MODULE * 0.9, 0.16, 0.13, 5, MAT_BRASS, 2.2],
];

const built = [];
for (let i = 0; i < SPECS.length; ++i) {
  const s = SPECS[i];
  const g = makeGear(s[0], s[1], s[2], s[3], s[4], 0.11, s[5], 2.8);
  const mesh = new THREE.Mesh(g.geometry, s[6]);
  mesh.name = s[0];
  scene.add(mesh);
  built.push({ mesh: mesh, pitchRadius: g.pitchRadius, teeth: s[1], module: s[2] });
}
for (let i = 0; i < IDLERS.length; ++i) {
  const s = IDLERS[i];
  const g = makeGear(s[0], s[1], s[2], s[3], s[4], 0.11, s[5], s[7]);
  const mesh = new THREE.Mesh(g.geometry, s[6]);
  mesh.name = s[0];
  scene.add(mesh);
  built.push({ mesh: mesh, pitchRadius: g.pitchRadius, teeth: s[1], module: s[2] });
}

// ---- 布局（与 bpy_watch.py 同公式）----
built[0].mesh.position.set(-1.2, 0.9, 0.0);
built[0].mesh.rotation.z = 0.0;

function placePair(prev, curr, angleDeg, baseZ) {
  const distance = prev.module * (prev.teeth + curr.teeth) / 2.0;
  const rad = (angleDeg * Math.PI) / 180.0;
  curr.mesh.position.set(
    prev.mesh.position.x + Math.cos(rad) * distance,
    prev.mesh.position.y + Math.sin(rad) * distance,
    baseZ
  );
  curr.mesh.rotation.z = rad + Math.PI / curr.teeth;
}
placePair(built[0], built[1], 205.0, 0.05);
placePair(built[1], built[2], 150.0, 0.10);
placePair(built[2], built[3], 95.0, 0.15);
placePair(built[3], built[4], 40.0, 0.20);

const idlerPositions = [[5.4, -3.2, 0.30], [-5.8, 3.4, 0.34], [6.6, 2.8, 0.26]];
for (let i = 0; i < 3; ++i) {
  built[5 + i].mesh.position.set(idlerPositions[i][0], idlerPositions[i][1],
                                 idlerPositions[i][2]);
  built[5 + i].mesh.rotation.z = i * 0.7;
}

scene;
