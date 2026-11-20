// ============================================================================
// 薄膜卷盘（卷盘式薄膜卷）· 三维还原
// ----------------------------------------------------------------------------
// 参考物：一只蓝色注塑塑料卷盘，三窗对称开孔；窗口内卷着暗红色薄膜，
// 薄膜表面压着一层银色网格肋条。盘面中央是凸起的轮毂圆盘。
//
// ## 坐标系（这决定了整份脚本）
//
// 盘片平放在 XY 平面，**+Z 是盘面法线（厚度方向）**。相机从 +Z 斜上方俯视，
// 相机 up 为默认 +Y ⇒ 画面里 +Y 朝上、+X 朝右，与参考图的俯拍取景一致。
//
// 三个开窗的中心角是 30° / 150° / 270°（左上、右上、正下），
// 顶部 90° 方向是实体区——标签与三角压印都在那里。
//
// ## 尺寸口径
//
// 全部以「盘片外缘半径 = 1.0」为单位（8 英寸盘的实际外径约 200mm，
// 即 1.0 单位 ≈ 100mm）。
// ============================================================================

const TAU = Math.PI * 2;
const DEG = Math.PI / 180;

// ── 主尺寸 ────────────────────────────────────────────────────────────────
const R_RIM = 1.000;          // 外缘翻边外半径
const R_PLATE = 0.952;        // 盘片本体外半径（外沿被翻边压住）
const Z_PLATE = 0.014;        // 盘片底面高度
const T_PLATE = 0.110;        // 盘片厚度
const Z_TOP = Z_PLATE + T_PLATE;   // 盘面高度 ≈ 0.124

const R_WIN_IN = 0.412;       // 窗口内半径
const R_WIN_OUT = 0.848;      // 窗口外半径
const WIN_HALF = 30 * DEG;    // 窗口半角
const WIN_A = [30 * DEG, 150 * DEG, 270 * DEG];

const R_HUB = 0.400;          // 中央轮毂圆盘半径
const Z_HUB_TOP = 0.160;      // 轮毂外圈顶面高度（比盘面高 0.036）

const Z_FILM_BOT = 0.030;     // 卷材底面
const Z_FILM_TOP = 0.090;     // 卷材顶面

// ============================================================================
// 相机 · 布光
// ============================================================================
const scene = new THREE.Scene();

// 俯视略带透视：不是正交正对，既保留圆盘形状又看得到厚度与轮毂高度
const camera = new THREE.PerspectiveCamera(36, 16 / 9, 0.05, 100);
camera.position.set(0.10, -0.88, 3.00);
camera.lookAt(0.0, -0.02, 0.06);
scene.add(camera);

scene.toneMapping = 4;   // ACES：塑料高光与金属反光都不会糊成一片白

scene.add(new THREE.AmbientLight(0x93a3b6, 0.55));

// 主光：右前上方（参考图的光来自画面右下，塑料表面有柔和高光）
const keyLight = new THREE.DirectionalLight(0xfff4e8, 2.45);
keyLight.position.set(1.9, -2.1, 3.4);
scene.add(keyLight);

// 补光：左侧冷色，压住暗部、不改主光方向
const fillLight = new THREE.DirectionalLight(0x9dbfe6, 0.85);
fillLight.position.set(-2.6, -0.9, 1.7);
scene.add(fillLight);

// 轮廓光：从后上方勾出外缘翻边的亮边
const rimLight = new THREE.DirectionalLight(0xffffff, 1.05);
rimLight.position.set(-1.4, 2.7, 1.5);
scene.add(rimLight);

// ============================================================================
// 材质
//
// 三种色彩语言：塑料蓝壳（低金属度、中粗糙度）/ 银色网格（高金属度、
// 低粗糙度，靠环境反射出亮）/ 暗红卷材（塑料膜）。
// ============================================================================
const MAT_SHELL = new THREE.MeshStandardMaterial({ color: 0x50719b, metalness: 0.06, roughness: 0.66 });
const MAT_HUB = new THREE.MeshStandardMaterial({ color: 0x52739c, metalness: 0.06, roughness: 0.62 });
const MAT_HUB_DARK = new THREE.MeshStandardMaterial({ color: 0x3a5578, metalness: 0.10, roughness: 0.66 });
const MAT_VOID = new THREE.MeshStandardMaterial({ color: 0x04060a, metalness: 0.0, roughness: 0.98 });
const MAT_WIRE = new THREE.MeshStandardMaterial({ color: 0xdfe5ea, metalness: 0.72, roughness: 0.30 });
// 卷材：注塑级红色薄膜。metalness 必须低——高金属度没有漫反射项，
// 凹在窗口里的卷材会直接变黑（实测：高金属度那一版窗口里一片死黑）
const MAT_FILM = new THREE.MeshStandardMaterial({ color: 0x77202c, metalness: 0.06, roughness: 0.46 });
const MAT_FILM_EDGE = new THREE.MeshStandardMaterial({ color: 0xa8737a, metalness: 0.24, roughness: 0.36 });
const MAT_LABEL = new THREE.MeshStandardMaterial({ color: 0xf1f1ea, metalness: 0.0, roughness: 0.66 });
const MAT_TAG = new THREE.MeshStandardMaterial({ color: 0x8c9aa6, metalness: 0.04, roughness: 0.70 });
const MAT_TAPE = new THREE.MeshStandardMaterial({ color: 0xd9dde0, metalness: 0.20, roughness: 0.38, transparent: true, opacity: 0.90 });
const MAT_INK = new THREE.MeshStandardMaterial({ color: 0x2a2f36, metalness: 0.0, roughness: 0.72 });
const MAT_GROUND = new THREE.MeshStandardMaterial({ color: 0x807b72, metalness: 0.0, roughness: 0.97 });

// ============================================================================
// 几何工具
// ============================================================================

// 极坐标「倒角扇形」轮廓：
//
// 参考图的窗口是**直边扇形**——两条径向边是真正的直线，只有四个角倒圆。
// 对极坐标矩形做超椭圆变形会把它变成“胶囊扇形”（边也鼓了），窗口会
// 明显偏胖。正确做法是逐边描：直边 + 每个角一段圆弧。
function windowPoints(aCenter, rIn, rOut, halfAngle, corner, steps) {
  const cr = corner || 0.026;
  const P = (a, r) => [Math.cos(a) * r, Math.sin(a) * r];
  const pts = [];

  // 内侧圆弧（从 -half+corner 角到 +half-corner）
  const ia0 = aCenter - halfAngle + cr / rIn;
  const ia1 = aCenter + halfAngle - cr / rIn;
  const nIn = Math.max(6, steps | 0);
  for (let i = 0; i <= nIn; i++) {
    pts.push(P(ia0 + (ia1 - ia0) * (i / nIn), rIn));
  }

  // 外侧圆弧（反向）
  const oa0 = aCenter + halfAngle - cr / rOut;
  const oa1 = aCenter - halfAngle + cr / rOut;
  const nOut = Math.max(6, steps | 0);
  for (let i = 0; i <= nOut; i++) {
    pts.push(P(oa0 + (oa1 - oa0) * (i / nOut), rOut));
  }

  return pts;
}

function shapeFromPoints(pts) {
  const sh = new THREE.Shape();
  sh.moveTo(pts[0][0], pts[0][1]);
  for (let i = 1; i < pts.length; i++) sh.lineTo(pts[i][0], pts[i][1]);
  sh.closePath();
  return sh;
}

// 旋转体：剖面 (半径, 轴向) 绕 Y 转一圈，再整体把轴从 Y 掰到 Z
function lathe(points, seg, mat, name) {
  const prof = [];
  for (let i = 0; i < points.length; i++) prof.push(new THREE.Vector2(points[i][0], points[i][1]));
  const mesh = new THREE.Mesh(new THREE.LatheGeometry(prof, seg), mat);
  mesh.rotation.x = Math.PI / 2;
  if (name) mesh.name = name;
  return mesh;
}

// 贴着盘面的平片（法线 +Z 的薄圆柱）
const discGeoCache = {};
function disc(r, h, mat, x, y, z, name) {
  const key = r.toFixed(4) + '|' + h.toFixed(4);
  let g = discGeoCache[key];
  if (!g) {
    g = new THREE.CylinderGeometry(r, r, h, 32);
    discGeoCache[key] = g;
  }
  const mesh = new THREE.Mesh(g, mat);
  mesh.rotation.x = Math.PI / 2;   // 圆柱轴向 Y → Z
  mesh.position.set(x, y, z);
  if (name) mesh.name = name;
  return mesh;
}

// 平面薄片（沿 X 长、沿 Y 宽、沿 Z 厚）
function slab(w, h, t, mat, x, y, z, rot) {
  const mesh = new THREE.Mesh(new THREE.BoxGeometry(w, h, t), mat);
  mesh.position.set(x, y, z);
  if (rot) mesh.rotation.z = rot;
  return mesh;
}

// ============================================================================
// 0. 地面（水泥地感：大平面 + 盘下的接触阴影）
// ============================================================================
{
  // 地面只铺盘周边一圈：自动取景按整体包围盒算，地布太大会把相机拉到
  // 几十单位外（实测 30 单位地面 → 取景距离 62.8，盘面缩成一小块）
  const ground = new THREE.Mesh(new THREE.PlaneGeometry(2.75, 2.75), MAT_GROUND);
  ground.position.z = -0.004;
  ground.name = 'ground';
  scene.add(ground);

  // 接触阴影：紧贴翻边外沿的两圈暗环
  scene.add(disc(1.075, 0.001, new THREE.MeshStandardMaterial({ color: 0x4a463f, metalness: 0.0, roughness: 1.0 }), 0.0, 0.0, 0.0004));
  scene.add(disc(1.030, 0.001, new THREE.MeshStandardMaterial({ color: 0x322f29, metalness: 0.0, roughness: 1.0 }), 0.0, 0.0, 0.0006));
}

// ============================================================================
// 1. 盘片本体 —— 外圆挤出 + 三个圆角扇形挖孔
//
// 挖孔而不是「画三根辐条」：窗口边界与盘面天然共面，不会出现辐条与盘面之间的缝。
// ============================================================================
{
  const plateShape = new THREE.Shape();
  plateShape.absarc(0, 0, R_PLATE, 0, TAU, false);
  for (let i = 0; i < WIN_A.length; i++) {
    plateShape.holes.push(shapeFromPoints(
      windowPoints(WIN_A[i], R_WIN_IN, R_WIN_OUT, WIN_HALF, 0.030, 46)));
  }
  const plateGeo = new THREE.ExtrudeGeometry(plateShape, {
    depth: T_PLATE,
    bevelEnabled: true,
    bevelThickness: 0.005,
    bevelSize: 0.005,
    bevelSegments: 2,
    curveSegments: 72
  });
  const plate = new THREE.Mesh(plateGeo, MAT_SHELL);
  plate.name = 'shell_plate';
  plate.position.z = Z_PLATE;
  scene.add(plate);
}

// ============================================================================
// 2. 外缘翻边 —— 旋转体剖面（外壁略鼓、上沿收圆）
// ============================================================================
{
  const rim = lathe([
    [0.906, 0.000],
    [0.986, 0.010],
    [1.000, 0.050],
    [0.996, 0.114],
    [0.960, 0.144],
    [0.916, 0.150],
    [0.904, 0.138],
    [0.904, 0.000]
  ], 144, MAT_SHELL, 'rim_flange');
  scene.add(rim);
}

// ============================================================================
// 3. 中央轮毂 —— 旋转体：外凸环 + 内平台 + 中心小凸台
//
// 参考图的结构是三层：外圈凸环（高于盘面）→ 圈内一圈略低的平台 →
// 中心一个小凸台。三个安装位（两个圆形 + 一个矩形凹槽）都在那层平台上。
// ============================================================================
{
  const hub = lathe([
    [0.000, Z_PLATE],
    [0.398, Z_PLATE],
    [0.400, 0.040],
    [0.400, 0.146],
    [0.394, Z_HUB_TOP],
    [0.330, Z_HUB_TOP],
    [0.322, 0.146],
    [0.176, 0.146],
    [0.176, 0.158],
    [0.150, 0.168],
    [0.086, 0.176],
    [0.000, 0.178]
  ], 144, MAT_HUB, 'hub_boss');
  scene.add(hub);

  // 轮毂外圈的一道细台阶（注塑件上的分型线，让轮毂与盘面区分开）
  const step = lathe([
    [0.404, 0.128],
    [0.412, 0.130],
    [0.412, 0.136],
    [0.404, 0.138]
  ], 144, MAT_SHELL, 'hub_step');
  scene.add(step);

  // ── 三瓣中心孔：中心圆 + 三个瓣（深色填充，视觉上等同挖穿）──
  const zVoid = 0.1795;
  scene.add(disc(0.030, 0.004, MAT_VOID, 0.0, 0.0, zVoid, 'center_bore'));
  for (let i = 0; i < 3; i++) {
    const a = 90 * DEG + i * 120 * DEG;
    scene.add(disc(0.021, 0.004, MAT_VOID, Math.cos(a) * 0.030, Math.sin(a) * 0.030, zVoid));
  }

  // ── 两个圆形凹槽（右上 30° / 右下 -60°）+ 一个矩形凹槽（左 180°）──
  const zPlate = 0.1468;
  const rr = 0.238;

  const roundA = [30 * DEG, -60 * DEG];
  for (let i = 0; i < roundA.length; i++) {
    const x = Math.cos(roundA[i]) * rr;
    const y = Math.sin(roundA[i]) * rr;
    scene.add(disc(0.072, 0.005, MAT_HUB_DARK, x, y, zPlate));
    const ring = new THREE.Mesh(new THREE.TorusGeometry(0.074, 0.0035, 6, 40), MAT_HUB);
    ring.position.set(x, y, zPlate + 0.0022);
    scene.add(ring);
    // 凹槽内的卡头：一小块斜摆的凸起
    scene.add(slab(0.052, 0.018, 0.012, MAT_HUB, x + 0.004, y - 0.010, zPlate + 0.007, roundA[i] - 0.5));
  }

  // 矩形凹槽（左）
  const rx = Math.cos(Math.PI) * rr, ry = 0.0;
  scene.add(slab(0.140, 0.098, 0.006, MAT_HUB_DARK, rx, ry, zPlate));
  scene.add(slab(0.148, 0.008, 0.008, MAT_HUB, rx, ry + 0.052, zPlate + 0.002));
  scene.add(slab(0.148, 0.008, 0.008, MAT_HUB, rx, ry - 0.052, zPlate + 0.002));
  scene.add(slab(0.008, 0.112, 0.008, MAT_HUB, rx - 0.070, ry, zPlate + 0.002));
  scene.add(slab(0.008, 0.112, 0.008, MAT_HUB, rx + 0.070, ry, zPlate + 0.002));
  scene.add(slab(0.024, 0.062, 0.014, MAT_HUB, rx + 0.028, ry, zPlate + 0.008, -0.25));
}

// ============================================================================
// 4. 三个窗口内的卷材 + 银色鳞甲
//
// 卷材是暗红塑料膜；肋条是银色金属，压在卷材表面之上、盘面之下，
// 于是从上方看进窗口时是「凹进去的一层鳞甲」——参考图正是这个层次。
// ============================================================================

// 四棱锥：ConeGeometry 用 4 个径向分段就是四棱锥
const bossGeo = new THREE.ConeGeometry(1, 1, 4, 1, false);

for (let w = 0; w < WIN_A.length; w++) {
  const aC = WIN_A[w];

  // ── 卷材本体（窗口内凹下去一层，周围留出塑料内壁）──
  const filmPts = windowPoints(aC, R_WIN_IN + 0.016, R_WIN_OUT - 0.016, WIN_HALF - 0.012, 0.030, 40);
  const filmGeo = new THREE.ExtrudeGeometry(shapeFromPoints(filmPts), {
    depth: Z_FILM_TOP - Z_FILM_BOT,
    bevelEnabled: false,
    curveSegments: 48
  });
  const filmMesh = new THREE.Mesh(filmGeo, MAT_FILM);
  filmMesh.name = 'film_coil_' + w;
  filmMesh.position.z = Z_FILM_BOT;
  scene.add(filmMesh);

  // 卷材顶面外圈的一圈浅色叠层：卷紧的膜侧壁反光
  const filmRim = new THREE.Mesh(
    new THREE.ExtrudeGeometry(shapeFromPoints(
      windowPoints(aC, R_WIN_OUT - 0.042, R_WIN_OUT - 0.022, WIN_HALF - 0.012, 0.030, 40)),
      { depth: 0.004, bevelEnabled: false, curveSegments: 40 }), MAT_FILM_EDGE);
  filmRim.position.z = Z_FILM_TOP;
  scene.add(filmRim);

  // ── 鳞甲格点 ──
  // 锥体几乎相接（内隙只留一条缝露出红膜）：参考图的鳞甲是密排的，
  // 锥体之间那些暗红细缝就是底下的卷材。
  const rows = 11;
  const perRow = [9, 10, 11, 12, 12, 12, 12, 11, 10, 9, 8];
  const r0 = R_WIN_IN + 0.018;
  const r1 = R_WIN_OUT - 0.018;
  const half = WIN_HALF - 0.014;
  const dr = (r1 - r0) / rows;

  for (let i = 0; i < rows; i++) {
    const rm = r0 + dr * (i + 0.5);
    const n = perRow[i];
    const cellT = (2 * half / n) * rm;
    const base = Math.min(cellT, dr) * 0.98;
    for (let s = 0; s < n; s++) {
      const ang = aC - half + 2 * half * ((s + (i % 2) * 0.5) / n);
      const boss = new THREE.Mesh(bossGeo, MAT_WIRE);
      boss.scale.set(base, 0.020, base);
      boss.position.set(Math.cos(ang) * rm, Math.sin(ang) * rm, Z_FILM_TOP + 0.010);
      // 四个面朝上下左右摆（与极坐标对齐，面片才对着光）
      boss.rotation.z = ang + Math.PI / 4;
      scene.add(boss);
    }
  }
}

// ============================================================================
// 5. 盘面顶部：打印标签 + 三角压印 + 两处胶带 + 一张白纸
// ============================================================================
{
  // ── 标签牌：灰底 ──
  const tagW = 0.404, tagH = 0.124;
  const tagY = 0.700, tagX = -0.010;
  scene.add(slab(tagW, tagH, 0.006, MAT_TAG, tagX, tagY, Z_TOP + 0.003));

  // ── 白色标签纸（叠在灰底上，四周露出一点底色）──
  const paperW = 0.366, paperH = 0.098;
  scene.add(slab(paperW, paperH, 0.004, MAT_LABEL, tagX - 0.004, tagY - 0.002, Z_TOP + 0.007));

  // ── 纸上的文字行（细深色条，长度不一 ⇒ 像排版过的说明） ──
  const lines = [
    [-0.152, 0.130, 0.0050],
    [-0.152, 0.118, 0.0050],
    [-0.152, 0.140, 0.0050],
    [0.030, 0.100, 0.0050],
    [-0.152, 0.124, 0.0040]
  ];
  for (let i = 0; i < lines.length; i++) {
    const lx = lines[i][0], lw = lines[i][1], lh = lines[i][2];
    scene.add(slab(lw, lh, 0.0022, MAT_INK, tagX + lx + lw * 0.5, tagY + 0.0332 - i * 0.0164, Z_TOP + 0.0094));
  }
  // 右侧几条短行（表格感）
  for (let i = 0; i < 3; i++) {
    scene.add(slab(0.052, 0.0044, 0.0022, MAT_INK, tagX + 0.088, tagY + 0.0200 - i * 0.0164, Z_TOP + 0.0094));
  }

  // ── 三角压印（顶部，标签与轮毂之间）：两层做出压印的台阶感 ──
  const triOuter = new THREE.Shape();
  triOuter.moveTo(0, 0.062);
  triOuter.lineTo(0.058, -0.040);
  triOuter.lineTo(-0.058, -0.040);
  triOuter.closePath();
  const triGeo = new THREE.ExtrudeGeometry(triOuter, { depth: 0.004, bevelEnabled: false });
  const triMesh = new THREE.Mesh(triGeo, MAT_SHELL);
  triMesh.position.set(0.0, 0.402, Z_TOP);
  triMesh.name = 'triangle_mark';
  scene.add(triMesh);

  const triIn = new THREE.Shape();
  triIn.moveTo(0, 0.040);
  triIn.lineTo(0.038, -0.026);
  triIn.lineTo(-0.038, -0.026);
  triIn.closePath();
  const triInMesh = new THREE.Mesh(
    new THREE.ExtrudeGeometry(triIn, { depth: 0.002, bevelEnabled: false }), MAT_HUB_DARK);
  triInMesh.position.set(0.0, 0.402, Z_TOP + 0.0036);
  scene.add(triInMesh);

  // ── 胶带：左上 / 右上（半透明灰，压在盘面上） ──
  scene.add(slab(0.120, 0.090, 0.003, MAT_TAPE, -0.660, 0.606, Z_TOP + 0.0028, 0.22));
  scene.add(slab(0.108, 0.086, 0.003, MAT_TAPE, 0.672, 0.480, Z_TOP + 0.0028, -0.10));

  // ── 底部一张白纸（搭在翻边顶部）──
  scene.add(slab(0.152, 0.108, 0.003, MAT_LABEL, 0.048, -0.905, 0.1520, 0.05));

  // ── 底部两个小圆标记（压印的风车符号，用细环 + 斜杆近似）──
  for (let i = 0; i < 2; i++) {
    const x = -0.075 + i * 0.150;
    const ring = new THREE.Mesh(new THREE.TorusGeometry(0.026, 0.0028, 6, 22), MAT_HUB_DARK);
    ring.position.set(x, -0.878, Z_TOP + 0.002);
    scene.add(ring);
    scene.add(slab(0.050, 0.0035, 0.003, MAT_HUB_DARK, x, -0.878, Z_TOP + 0.002, 0.7 + i * 0.5));
  }
}

// ============================================================================
// 6. 收尾
// ============================================================================
scene;
