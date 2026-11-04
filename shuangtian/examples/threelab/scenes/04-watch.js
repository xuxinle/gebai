// ============================================================================
// 发条机械表 · 精机剖面
// ----------------------------------------------------------------------------
// 一个把 three.js 程序化几何能力用满的场景：
//
//   · 齿轮齿形 —— 数学（齿根弧 + 齿腹斜线 + 齿顶弧），不是贴图
//   · 轮系结构 —— 齿圈 / 轮辐 / 轴孔三层，全部循环生成
//   · 齿轮啮合 —— 位置由模数与齿数算出，不是手调
//   · 游丝     —— 螺旋曲线 + TubeGeometry
//   · 摆轮     —— 振荡动画（userData.oscillate）
//   · 指针     —— 父子层级 + 各自自转（userData.spin）
//   · 表壳     —— LatheGeometry 旋转体 + ExtrudeGeometry 表耳
//   · 表盘     —— 刻度环、秒轨
//
// 改一个数字（比如齿数），整张图跟着变。
//
// ## 构图约定（这决定了整份脚本的坐标系）
//
// **Z 轴从下往上**，越靠上越"靠近观察者"：
//
//     z ≈  3.4   指针
//     z ≈  2.3   夹板环 + 螺丝
//     z ≈  1.3   轮系（齿轮 / 摆轮 / 游丝）
//     z ≈  1.0   表盘刻度环（**只有外圈**，中间镂空——机芯从洞里露出来）
//     z ≈  0.0   主夹板底面
//     z ≈ -1.0   表壳底
//
// 表盘做成镂空环是本场景的关键设计：真实表盘是实心盘，会把轮系全遮住；
// 而这里要展示的正是轮系。
// ============================================================================

const scene = new THREE.Scene();

// ── 尺寸约定（单位毫米级，表径 40）──────────────────────────────────────────
const R_CASE = 20.0;        // 表壳外半径
const R_DIAL_OUT = 17.6;    // 表盘外半径
const R_DIAL_IN = 8.4;      // 表盘内半径（镂空处的边界）
const R_PLATE = 13.6;       // 主夹板 / 夹板环半径
const Z_DIAL = 1.00;        // 表盘平面
const Z_GEAR = 1.32;        // 轮系层
const Z_BRIDGE = 2.32;      // 夹板层
const Z_HANDS = 3.40;       // 指针层

// ============================================================================
// 相机 · 三点布光
// ============================================================================
const camera = new THREE.PerspectiveCamera(30, 16 / 9, 1.0, 400);
camera.position.set(10, 16, 84);
// ## 用 quaternion 而不用 lookAt（实测踩到的 QuickJS 数值缺陷）
//
// three 的 lookAt() 内部走 Matrix4.lookAt → setFromRotationMatrix，
// 在 QuickJS 里**斜向视角会角度减半**（指向 45° 的位置只转出 ~22°），
// 而直接写 quaternion.setFromAxisAngle 是准的。
//
// 症状：脚本里写的视角完全不起作用，画面停在一个默认朝向上——
// 而 three_check 与视口用的是同一份抽取，所以两边一起错，很难察觉。
const camYaw = Math.atan2(10, 84);                      // 水平偏角
const camPitch = Math.atan2(16, Math.hypot(10, 84));    // 俯角
camera.quaternion.setFromAxisAngle(new THREE.Vector3(0, 1, 0), camYaw);
camera.quaternion.multiply(
  new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(1, 0, 0), -camPitch));
scene.add(camera);

scene.add(new THREE.AmbientLight(0x6a7a95, 0.72));

// 主光：右上偏硬（金属齿形要靠方向性光才有明暗层次）
const key = new THREE.DirectionalLight(0xfff4e6, 2.9);
key.position.set(26, 32, 46);
key.target.position.set(0, 0, 1.5);
scene.add(key, key.target);

// 补光：左上冷色，压住暗部但别吃掉对比
const fill = new THREE.DirectionalLight(0x8fb4ff, 1.5);
fill.position.set(-36, 20, 22);
scene.add(fill);

// 轮廓光：背后下侧，让表壳边缘从暗背景里浮出来
const rim = new THREE.DirectionalLight(0xffd9a0, 1.9);
rim.position.set(-12, -26, -32);
scene.add(rim);

// ============================================================================
// 材质
// ============================================================================
// 主夹板：枪灰（真实机芯的夹板镀铑/镀镍，冷灰偏蓝）
const MAT_PLATE = new THREE.MeshStandardMaterial({
  color: 0x5c6570, metalness: 0.9, roughness: 0.46
});
// 齿轮：黄铜，粗糙度略高以显出"机加工"而不是镜面
const MAT_BRASS = new THREE.MeshStandardMaterial({
  color: 0xc4974a, metalness: 0.95, roughness: 0.30
});
// 次级轮：偏红的铜，与主轮分层
const MAT_COPPER = new THREE.MeshStandardMaterial({
  color: 0xa2673a, metalness: 0.9, roughness: 0.40
});
// 钢件：摆轮、螺丝槽
const MAT_STEEL = new THREE.MeshStandardMaterial({
  color: 0xcfd5dd, metalness: 1.0, roughness: 0.16
});
// 指针：抛光白金（比钢更亮更暖一点——深蓝表盘上要“跳”得出来）
const MAT_HAND = new THREE.MeshStandardMaterial({
  color: 0xf2f5f9, metalness: 0.97, roughness: 0.10
});
// 蓝钢（制表里的氧化着色：蓝钢螺丝/指针/游丝）
const MAT_BLUED = new THREE.MeshStandardMaterial({
  color: 0x2c4a86, metalness: 0.88, roughness: 0.28
});
// 表壳：不锈钢
const MAT_CASE = new THREE.MeshStandardMaterial({
  color: 0x7d858f, metalness: 0.92, roughness: 0.42
});
// 表盘：深蓝哑光
const MAT_DIAL = new THREE.MeshStandardMaterial({
  color: 0x1e2c4a, metalness: 0.35, roughness: 0.62
});
// 红宝石轴眼
const MAT_JEWEL = new THREE.MeshStandardMaterial({
  color: 0x9e1428, metalness: 0.15, roughness: 0.18,
  emissive: 0x400008, emissiveIntensity: 0.7
});

// ============================================================================
// 齿形生成器
// ============================================================================
//
// ## 齿形怎么来的
//
// 真实齿轮是渐开线齿廓（要解基圆、压力角、变位系数）。对**视觉**场景
// 那是过度设计。这里用"齿根弧 + 齿腹斜线 + 齿顶弧"的近似：
//
//   · 齿顶与齿根是**圆弧** —— 否则转动起来像锯齿块
//   · 齿腹是**斜直线** —— 压力角的视觉代理
//   · 齿的相位由 `2π/teeth` 均分 —— 于是啮合位置一算就真的能咬上
//
// 返回 `pitchRadius`（分度圆），供调用方算中心距：
// 两轮中心距 = 模数 × (z1 + z2) / 2
//
function makeGear(options) {
  const {
    teeth = 24,
    module: m = 0.21,            // 模数：分度圆直径 = 模数 × 齿数
    rimWidth = 0.18,             // 齿高（径向跨度）
    depth = 0.14,                // 轴向厚度
    spokes = 5,                  // 轮辐数（0 = 实心盘）
    bore = 0.11,                 // 中心轴孔
    holeScale = 1.0              // 轮辐窗口大小系数
  } = options;

  const pitchRadius = (m * teeth) / 2;
  const tipRadius = pitchRadius + rimWidth * 0.5;
  const rootRadius = pitchRadius - rimWidth * 0.5;
  const pitchAngle = (Math.PI * 2) / teeth;
  const toothAngle = pitchAngle * 0.44;   // 齿宽（留侧隙，否则看不见齿缝）

  // ── 外轮廓：沿齿圈走一圈 ──
  const shape = new THREE.Shape();
  const profile = [];
  for (let i = 0; i < teeth; i++) {
    const base = i * pitchAngle;
    const a0 = base - toothAngle * 0.5;
    const a1 = base - toothAngle * 0.30;
    const a2 = base + toothAngle * 0.30;
    const a3 = base + toothAngle * 0.5;
    profile.push([Math.cos(a0) * rootRadius, Math.sin(a0) * rootRadius]);
    profile.push([Math.cos(a1) * tipRadius, Math.sin(a1) * tipRadius]);
    profile.push([Math.cos(a2) * tipRadius, Math.sin(a2) * tipRadius]);
    profile.push([Math.cos(a3) * rootRadius, Math.sin(a3) * rootRadius]);
    // 齿间空档：用几段折线近似齿根圆弧
    const gapStart = a3;
    const gapEnd = base + pitchAngle - toothAngle * 0.5;
    for (let s = 1; s <= 3; s++) {
      const a = gapStart + (gapEnd - gapStart) * (s / 3);
      profile.push([Math.cos(a) * rootRadius, Math.sin(a) * rootRadius]);
    }
  }
  shape.moveTo(profile[0][0], profile[0][1]);
  for (let i = 1; i < profile.length; i++) shape.lineTo(profile[i][0], profile[i][1]);
  shape.closePath();

  // ── 轮辐窗口（挖孔，而不是画辐条）──
  //
  // 挖孔的好处：几何量固定（与辐条数无关），且窗口边界与齿圈天然共面，
  // 不会出现"辐条与轮缘之间有缝"。
  const hub = Math.max(bore * 2.8, rootRadius * 0.26);
  const windowOuter = rootRadius - rimWidth * 1.15;
  const windowInner = hub * 1.45;
  if (spokes > 0 && windowOuter > windowInner + 0.04) {
    const span = (Math.PI * 2) / spokes;
    // 辐条宽度（角向占比）：轮辐少的齿轮辐条宽一些才像真的
    const spokeGap = span * (0.34 / Math.max(1, spokes / 5));
    for (let s = 0; s < spokes; s++) {
      const start = s * span + spokeGap * 0.5;
      const end = (s + 1) * span - spokeGap * 0.5;
      const hole = new THREE.Path();
      const steps = 9;
      const ro = windowOuter * holeScale;
      const ri = windowInner;
      for (let k = 0; k <= steps; k++) {
        const a = start + (end - start) * (k / steps);
        const px = Math.cos(a) * ro;
        const py = Math.sin(a) * ro;
        if (k === 0) hole.moveTo(px, py);
        else hole.lineTo(px, py);
      }
      for (let k = steps; k >= 0; k--) {
        const a = start + (end - start) * (k / steps);
        hole.lineTo(Math.cos(a) * ri, Math.sin(a) * ri);
      }
      hole.closePath();
      shape.holes.push(hole);
    }
  }

  // 中心轴孔
  const borePath = new THREE.Path();
  borePath.absarc(0, 0, bore, 0, Math.PI * 2, true);
  shape.holes.push(borePath);

  const geometry = new THREE.ExtrudeGeometry(shape, {
    depth,
    bevelEnabled: true,
    bevelThickness: depth * 0.12,
    bevelSize: 0.010,
    bevelSegments: 1,
    curveSegments: 4
  });
  geometry.computeVertexNormals();
  return { geometry, pitchRadius, tipRadius };
}

// 往父级里加一个齿轮（含穿过它的蓝钢轴）
function addGear(parent, spec) {
  const { geometry, pitchRadius } = makeGear(spec);
  const mesh = new THREE.Mesh(geometry, spec.material || MAT_BRASS);
  mesh.name = spec.name;
  mesh.position.set(spec.x || 0, spec.y || 0, spec.z === undefined ? Z_GEAR : spec.z);
  if (spec.phase) mesh.rotation.z = spec.phase;
  if (spec.spin !== undefined) mesh.userData.spin = { axis: [0, 0, 1], speed: spec.spin };
  parent.add(mesh);

  if (spec.arbor !== false) {
    const arbor = new THREE.Mesh(
      new THREE.CylinderGeometry((spec.bore || 0.11) * 0.85, (spec.bore || 0.11) * 0.85,
                                 (spec.depth || 0.14) * 3.0, 14),
      MAT_BLUED);
    arbor.name = mesh.name + '_arbor';
    arbor.rotation.x = Math.PI / 2;
    arbor.position.copy(mesh.position);
    parent.add(arbor);
  }
  return { mesh, pitchRadius };
}

// ============================================================================
// 1. 表壳 —— LatheGeometry 旋转体（剖面轮廓绕轴转一圈）
// ============================================================================
{
  // 剖面从**内上沿**开始，向外下、绕过外壁、再回到内下沿——一个**壳**而非实心盘。
  //
  // ⚠ 第一版从 `[0, -0.95]`（中心点）起始，`LatheGeometry` 会扫出一个
  // **实心盘**——而它正对着相机，于是视口里看到的是一块白盘子，
  // 里面 129 个零件全被挡住。症状是“画面有东西但完全不对”，
  // 比“什么都没有”更难意识到问题出在表壳上。
  const profile = [
    [R_DIAL_OUT * 0.90, 1.02],  // 内上沿（表面开口）
    [R_DIAL_OUT * 1.02, 0.94],
    [R_CASE * 0.955, 0.86],     // 表圈（只比表盘宽一点）
    [R_CASE * 0.98, 0.66],
    [R_CASE, 0.30],             // 侧壁（薄）
    [R_CASE * 0.985, -0.34],
    [R_CASE * 0.90, -0.78],
    [R_CASE * 0.60, -0.95],
    [R_PLATE * 0.42, -0.95]     // 内下沿（底也是开口）
  ].map(([r, z]) => new THREE.Vector2(r, z));
  const caseMesh = new THREE.Mesh(new THREE.LatheGeometry(profile, 120), MAT_CASE);
  caseMesh.name = 'case';
  // ## LatheGeometry 绕 **Y 轴** 旋转，而本场景 Z 轴才是厚度方向
  //
  // 这是实测踩到的坐标系缺陷：profile 的 (r, z) 被解释成 (x, y)，绕 Y 转出来的
  // 旋转体其**轴向是 Y**（盘面躺在 XZ 平面）。而表盘/齿轮用的是 ExtrudeGeometry
  //（盘面在 XY 平面）。两者**互相垂直**——于是表壳变成一个"平躺的托盘"，
  // 机芯像一块板竖直插在托盘上。
  //
  // 修法：绕 X 轴转 90°，把 Y 轴转到 Z 上。此后 profile 的第二个分量就真的是高度。
  // ## 表壳整体往 -Z 退，让它的开口刚好包住表盘
  //
  // Lathe 的 profile 第二个分量在 `rotation.x = PI/2` 之后变成**世界 Z**，
  // 范围是 -0.95..1.02。而表盘/轮系/指针分别在 z = 1.0 / 1.32 / 3.4——
  // 于是**表壳把内容夹在了自己的壁里**，从前看就是一整块白盘。
  //
  // 实测现象：拉近镜头能看到齿轮与摆轮的轮廓透出来，但被一层白面盖住。
  //
  // 退后 1.5：表壳内上沿到 z = 1.02 - 1.5 = -0.48，不再碰到表盘；
  // 同时它仍从侧面抱住整个机芯（表壳半径 20 > 机芯半径 13.6）。
  caseMesh.position.z = -1.05;
  caseMesh.rotation.x = Math.PI / 2;
  scene.add(caseMesh);

  // 表耳 × 4（ExtrudeGeometry：矩形轮廓挤出）
  //
  // 位置从表壳侧壁**向外长**——第一版放在 `R_CASE - 0.6` 处，
  // 结果是四块悬空的方块（不与表壳相连）。
  // 表耳轮廓：一段从表壳侧壁伸出、末端略收窄的“耳”
  const lugShape = new THREE.Shape();
  lugShape.moveTo(0, 0);
  lugShape.lineTo(2.2, 0);
  lugShape.lineTo(2.2, 1.2);
  lugShape.lineTo(1.3, 1.2);
  lugShape.lineTo(1.3, 2.6);
  lugShape.lineTo(0, 2.6);
  lugShape.closePath();
  const lugGeo = new THREE.ExtrudeGeometry(lugShape, {
    depth: 0.72, bevelEnabled: true, bevelThickness: 0.06,
    bevelSize: 0.06, bevelSegments: 1
  });
  lugGeo.computeVertexNormals();
  // ## 表耳：4 只，分别贴在 12 点与 6 点两侧（真实手表的表带接口）
  //
  // 关键在**旋转**：轮廓是沿 +Y 长出的（0..2.6），而表壳是绕 Z 的圆盘，
  // 所以“朝外”就是沿 ±Y。第一版把它们按 45° 摆成四只脚、后来又忘了
  // 把轮廓的 +Y 方向对准径向，结果看起来是四个悬空的 L 形。
  //
  // 这里用极角直接算：12 点的耳在 ±(90°∓12°) 方向，6 点在 ±(270°∓12°)。
  const lugAngles = [
    Math.PI / 2 - 0.18, Math.PI / 2 + 0.18,   // 12 点附近的两只
    -Math.PI / 2 - 0.18, -Math.PI / 2 + 0.18  // 6 点附近的两只
  ];
  lugAngles.forEach((angle, i) => {
    const lug = new THREE.Mesh(lugGeo, MAT_CASE);
    lug.name = 'lug_' + i;
    // 根部略微埋进侧壁（不留缝），径向朝外
    const r = R_CASE * 0.96;
    lug.position.set(Math.cos(angle) * r, Math.sin(angle) * r, -0.18);
    // 轮廓的 +Y → 径向：rotation.z = angle - 90°
    lug.rotation.z = angle - Math.PI / 2;
    scene.add(lug);
  });
}

// ============================================================================
// 2. 主夹板底面 —— 整块机芯的"地板"
// ============================================================================
{
  // ## 为什么主夹板做成**环**而不是整块盘
  //
  // 真实机芯的主夹板是整块（轮系夹在两面夹板之间）。但在**剖面展示**里，
  // 一块整盘会把下面的表盘与背景全部遮住，画面变成"一大片灰"——
  // 实测截图里就是如此，层次全丢。
  //
  // 做成环之后：轮系仍坐在金属上（有支撑感），而中间能透出表盘的深蓝。
  const plateProfile = [
    [R_PLATE * 0.30, -0.30],
    [R_PLATE * 0.92, -0.30],
    [R_PLATE * 0.99, -0.14],
    [R_PLATE, 0.06],
    [R_PLATE, 0.18],
    [R_PLATE * 0.965, 0.24],
    [R_PLATE * 0.30, 0.24]
  ].map(([r, z]) => new THREE.Vector2(r, z));
  const plate = new THREE.Mesh(new THREE.LatheGeometry(plateProfile, 88), MAT_PLATE);
  plate.name = 'mainplate';
  plate.rotation.x = Math.PI / 2;   // 同上：Lathe 绕 Y，统一转到 Z 向上
  scene.add(plate);
}

// ============================================================================
// 3. 表盘 —— 只留外圈刻度环（中间镂空，让机芯露出来）
// ============================================================================
{
  const ringShape = new THREE.Shape();
  ringShape.absarc(0, 0, R_DIAL_OUT, 0, Math.PI * 2, false);
  const hole = new THREE.Path();
  hole.absarc(0, 0, R_DIAL_IN, 0, Math.PI * 2, true);
  ringShape.holes.push(hole);
  const dialGeo = new THREE.ExtrudeGeometry(ringShape, {
    depth: 0.16, bevelEnabled: false, curveSegments: 88
  });
  dialGeo.computeVertexNormals();
  const dial = new THREE.Mesh(dialGeo, MAT_DIAL);
  dial.name = 'dial_ring';
  dial.position.z = Z_DIAL - 0.08;
  scene.add(dial);

  // 秒轨（不锈钢细环）
  const trackProfile = [
    [R_DIAL_OUT - 0.62, 0],
    [R_DIAL_OUT - 0.50, 0],
    [R_DIAL_OUT - 0.50, 0.09],
    [R_DIAL_OUT - 0.62, 0.09]
  ].map(([r, z]) => new THREE.Vector2(r, z));
  const track = new THREE.Mesh(new THREE.LatheGeometry(trackProfile, 88), MAT_STEEL);
  track.name = 'seconds_track';
  track.rotation.x = Math.PI / 2;
  track.position.z = Z_DIAL + 0.02;
  scene.add(track);

  // ── 时标：12 个立体罗马数字位（用简单的棱柱拼，不用字体）──
  //
  // ## 为什么不用 TextGeometry

  // three 的 TextGeometry 需要字体 JSON（typeface），而本框架的三维层
  // **不加载外部资源**（无浏览器环境）。所以这里用「几何拼数字」的办法：
  // 12 个位置各放一个小棱柱，形状按该时刻的经典符号简化——
  // 观感上足够像时标，而且完全程序化、零外部依赖。
  //
  // 想要真的罗马数字，可以用若干个细长 BoxGeometry 拼（I 一根 / V 两根…），
  // 这里为了顶点数控制在「一根棱柱 + 不同缩放」的简化方案。
  const hourMarkers = [
    { r: 12.6, w: 0.60, h: 1.30 },   // 12
    { r: 12.6, w: 0.42, h: 0.80 },   // 1
    { r: 12.6, w: 0.42, h: 0.80 },   // 2
    { r: 12.6, w: 0.60, h: 1.10 },   // 3
    { r: 12.6, w: 0.42, h: 0.80 },   // 4
    { r: 12.6, w: 0.42, h: 0.80 },   // 5
    { r: 12.6, w: 0.60, h: 1.30 },   // 6
    { r: 12.6, w: 0.42, h: 0.80 },   // 7
    { r: 12.6, w: 0.42, h: 0.80 },   // 8
    { r: 12.6, w: 0.60, h: 1.10 },   // 9
    { r: 12.6, w: 0.42, h: 0.80 },   // 10
    { r: 12.6, w: 0.42, h: 0.80 }    // 11
  ];
  hourMarkers.forEach((mk, i) => {
    // 12 点在 +Y，顺时针递增 → 角度 = -90° + i*30°（用数学角表示）
    const angle = Math.PI / 2 - (i / 12) * Math.PI * 2;
    const marker = new THREE.Mesh(
      new THREE.BoxGeometry(mk.w, mk.h, 0.10), MAT_HAND);
    marker.name = 'hour_marker_' + i;
    marker.position.set(Math.cos(angle) * mk.r, Math.sin(angle) * mk.r, Z_DIAL + 0.06);
    marker.rotation.z = angle - Math.PI / 2;
    scene.add(marker);
  });
  // 刻度：60 道，整点更长更粗；每 15 分最粗
  const tickGeo = new THREE.BoxGeometry(0.09, 1, 0.055);
  for (let i = 0; i < 60; i++) {
    const angle = (i / 60) * Math.PI * 2;
    const isHour = i % 5 === 0;
    const isQuarter = i % 15 === 0;
    const tick = new THREE.Mesh(tickGeo, isHour ? MAT_STEEL : MAT_BLUED);
    tick.name = 'tick_' + i;
    const radius = R_DIAL_OUT - 1.05;
    tick.position.set(Math.cos(angle) * radius, Math.sin(angle) * radius, Z_DIAL + 0.05);
    tick.rotation.z = angle - Math.PI / 2;
    tick.scale.set(isQuarter ? 2.1 : isHour ? 1.6 : 0.95,
                   isQuarter ? 1.55 : isHour ? 1.05 : 0.52, 1);
    scene.add(tick);
  }
}

// ============================================================================
// 4. 轮系 —— 程序化齿轮 + 按模数算啮合位置
// ============================================================================
//
// ## 啮合位置怎么算
//
// 模数相同的两轮：中心距 = 模数 × (z1 + z2) / 2。
// 给定第一个轮的位置与第二个轮的方向角，第二个位置就唯一确定。
//
// 比"目测摆位置"强的地方：齿真的能咬上，改齿数时位置自动跟着走。
// 转速比同样来自齿数比（反向）。
//
const gearRoot = new THREE.Group();
gearRoot.name = 'gear_train';
scene.add(gearRoot);

const MODULE = 0.245;

// 主轮（发条盒大轮）
const barrel = addGear(gearRoot, {
  name: 'barrel_wheel', teeth: 42, module: MODULE, rimWidth: 0.34, depth: 0.30,
  spokes: 0, bore: 0.22, x: -1.4, y: -1.6, material: MAT_BRASS, spin: 0.055
});

// 二轮：与主轮啮合
const d1 = (MODULE * (42 + 26)) / 2;
const a1 = THREE.MathUtils.degToRad(46);
const center = addGear(gearRoot, {
  name: 'center_wheel', teeth: 26, module: MODULE, rimWidth: 0.24, depth: 0.26,
  spokes: 5, bore: 0.13,
  x: barrel.mesh.position.x + Math.cos(a1) * d1,
  y: barrel.mesh.position.y + Math.sin(a1) * d1,
  z: Z_GEAR + 0.03, material: MAT_BRASS,
  spin: -0.055 * (42 / 26)
});

// 三轮
const d2 = (MODULE * (26 + 18)) / 2;
const a2 = THREE.MathUtils.degToRad(-62);
const third = addGear(gearRoot, {
  name: 'third_wheel', teeth: 18, module: MODULE, rimWidth: 0.20, depth: 0.22,
  spokes: 4, bore: 0.11,
  x: center.mesh.position.x + Math.cos(a2) * d2,
  y: center.mesh.position.y + Math.sin(a2) * d2,
  z: Z_GEAR + 0.06, material: MAT_COPPER,
  spin: 0.055 * (42 / 18)
});

// 四轮（秒轮）
const d3 = (MODULE * (18 + 14)) / 2;
const a3 = THREE.MathUtils.degToRad(28);
const fourth = addGear(gearRoot, {
  name: 'fourth_wheel', teeth: 14, module: MODULE, rimWidth: 0.17, depth: 0.20,
  spokes: 4, bore: 0.10,
  x: third.mesh.position.x + Math.cos(a3) * d3,
  y: third.mesh.position.y + Math.sin(a3) * d3,
  z: Z_GEAR + 0.09, material: MAT_BRASS,
  spin: -0.055 * (42 / 14)
});

// 擒纵轮（齿形更尖、转速最快）
const d4 = (MODULE * (14 + 15)) / 2;
const a4 = THREE.MathUtils.degToRad(118);
addGear(gearRoot, {
  name: 'escape_wheel', teeth: 15, module: MODULE * 0.86, rimWidth: 0.15, depth: 0.13,
  spokes: 4, bore: 0.08,
  x: fourth.mesh.position.x + Math.cos(a4) * d4,
  y: fourth.mesh.position.y + Math.sin(a4) * d4,
  z: Z_GEAR + 0.12, material: MAT_STEEL,
  spin: 0.055 * (42 / 15) * 1.7
});

// 几个过渡小轮（真实机芯上确实还有若干）
[
  { teeth: 12, x: 5.6, y: 5.4, rate: -1.1, mat: MAT_BRASS },
  { teeth: 10, x: 7.6, y: 3.0, rate: 1.5, mat: MAT_COPPER },
  { teeth: 16, x: -6.8, y: 5.0, rate: 0.34, mat: MAT_BRASS }
].forEach((e, i) => {
  addGear(gearRoot, {
    name: 'idler_' + i, teeth: e.teeth, module: MODULE * 0.9, rimWidth: 0.16,
    depth: 0.14, spokes: 4, bore: 0.09,
    x: e.x, y: e.y, z: Z_GEAR + 0.05, material: e.mat,
    spin: 0.055 * e.rate * 18
  });
});

// ============================================================================
// 5. 擒纵机构 —— 摆轮 + 游丝 + 擒纵叉（机械表的心脏）
// ============================================================================
const escapement = new THREE.Group();
escapement.name = 'escapement';
escapement.position.set(5.0, -6.2, Z_GEAR + 0.16);
scene.add(escapement);

// 摆轮：环 + 三辐（真实摆轮就是"环 + 辐"，中间镂空）
{
  const R_BAL = 2.75;
  const RIM = 0.42;
  const HUB = 0.44;
  const spokeShape = new THREE.Shape();
  spokeShape.absarc(0, 0, R_BAL, 0, Math.PI * 2, false);
  // 先挖掉环内大半（只留环）
  const innerRing = new THREE.Path();
  innerRing.absarc(0, 0, R_BAL - RIM, 0, Math.PI * 2, true);
  spokeShape.holes.push(innerRing);
  // 再挖三个扇形，留下三根辐条
  const spokes = 3;
  const span = (Math.PI * 2) / spokes;
  const spokeWidth = span * 0.15;
  for (let s = 0; s < spokes; s++) {
    const start = s * span + spokeWidth * 0.5;
    const end = (s + 1) * span - spokeWidth * 0.5;
    const hole = new THREE.Path();
    const steps = 12;
    for (let k = 0; k <= steps; k++) {
      const a = start + (end - start) * (k / steps);
      const px = Math.cos(a) * (R_BAL - RIM);
      const py = Math.sin(a) * (R_BAL - RIM);
      if (k === 0) hole.moveTo(px, py);
      else hole.lineTo(px, py);
    }
    for (let k = steps; k >= 0; k--) {
      const a = start + (end - start) * (k / steps);
      hole.lineTo(Math.cos(a) * HUB, Math.sin(a) * HUB);
    }
    hole.closePath();
    spokeShape.holes.push(hole);
  }
  const balGeo = new THREE.ExtrudeGeometry(spokeShape, {
    depth: 0.20, bevelEnabled: true, bevelThickness: 0.03,
    bevelSize: 0.018, bevelSegments: 1, curveSegments: 26
  });
  balGeo.computeVertexNormals();
  const balance = new THREE.Mesh(balGeo, MAT_STEEL);
  balance.name = 'balance_wheel';
  // 摆轮**振荡**（不是匀速转）：±0.92 rad ≈ ±53°，3.2 Hz
  balance.userData.oscillate = { axis: [0, 0, 1], amplitude: 0.92, frequency: 3.2 };
  escapement.add(balance);

  // 摆轮轴
  const staff = new THREE.Mesh(new THREE.CylinderGeometry(0.07, 0.07, 1.4, 12), MAT_STEEL);
  staff.name = 'balance_staff';
  staff.rotation.x = Math.PI / 2;
  escapement.add(staff);

  // 配重螺钉（蓝钢，一圈 8 颗；真实摆轮上用于调快慢）
  for (let i = 0; i < 8; i++) {
    const a = (i / 8) * Math.PI * 2;
    const screw = new THREE.Mesh(
      new THREE.CylinderGeometry(0.072, 0.072, 0.24, 10), MAT_BLUED);
    screw.name = 'balance_screw_' + i;
    screw.rotation.x = Math.PI / 2;
    screw.position.set(Math.cos(a) * (R_BAL - RIM * 0.5),
                       Math.sin(a) * (R_BAL - RIM * 0.5), 0.15);
    balance.add(screw);
  }
}

// 游丝：阿基米德螺旋 → CatmullRomCurve3 → TubeGeometry
//
// ## 为什么不用 Line
//
// 折线在三维里**没有厚度**，光照完全无效，看起来像贴纸。
// 管子有真正的圆柱截面，与钢的材质一致（代价是顶点数：这里约 7 千）。
{
  const turns = 10;
  const rStart = 0.36;
  const rEnd = 1.62;
  const steps = turns * 28;
  const points = [];
  for (let i = 0; i <= steps; i++) {
    const t = i / steps;
    const angle = t * Math.PI * 2 * turns;
    const radius = rStart + (rEnd - rStart) * t;   // 等距螺旋
    const z = Math.sin(t * Math.PI * 6) * 0.03;    // 轻微起伏（呼吸感）
    points.push(new THREE.Vector3(Math.cos(angle) * radius, Math.sin(angle) * radius, z));
  }
  const curve = new THREE.CatmullRomCurve3(points, false, 'catmullrom', 0.25);
  const springGeo = new THREE.TubeGeometry(curve, steps, 0.030, 7, false);
  const spring = new THREE.Mesh(springGeo, MAT_BLUED);
  spring.name = 'hairspring';
  spring.position.z = 0.28;
  spring.userData.oscillate = { axis: [0, 0, 1], amplitude: 0.50, frequency: 3.2 };
  escapement.add(spring);
}

// 擒纵叉（pallet fork）：Y 形件，与摆轮同频反相摆动
{
  const forkShape = new THREE.Shape();
  forkShape.moveTo(-0.15, 0);
  forkShape.lineTo(-0.09, 0.85);
  forkShape.lineTo(-0.50, 1.34);
  forkShape.lineTo(-0.32, 1.50);
  forkShape.lineTo(0.05, 1.00);
  forkShape.lineTo(0.41, 1.38);
  forkShape.lineTo(0.58, 1.20);
  forkShape.lineTo(0.15, 0.58);
  forkShape.lineTo(0.15, 0);
  forkShape.closePath();
  const forkGeo = new THREE.ExtrudeGeometry(forkShape, {
    depth: 0.12, bevelEnabled: true, bevelThickness: 0.02,
    bevelSize: 0.014, bevelSegments: 1
  });
  forkGeo.computeVertexNormals();
  const fork = new THREE.Mesh(forkGeo, MAT_STEEL);
  fork.name = 'pallet_fork';
  fork.position.set(-3.1, 1.3, -0.04);
  // 同频、反相、振幅更小
  fork.userData.oscillate = { axis: [0, 0, 1], amplitude: 0.20, frequency: 3.2,
                              phase: Math.PI };
  escapement.add(fork);
}

// ============================================================================
// 6. 夹板环 + 螺丝 + 红宝石轴眼
// ============================================================================
{
  // 细环（只压住轮系外沿，不遮挡齿形）
  const ringProfile = [
    [R_PLATE * 0.90, Z_BRIDGE],
    [R_PLATE, Z_BRIDGE],
    [R_PLATE, Z_BRIDGE + 0.22],
    [R_PLATE * 0.90, Z_BRIDGE + 0.22]
  ].map(([r, z]) => new THREE.Vector2(r, z));
  const bridge = new THREE.Mesh(new THREE.LatheGeometry(ringProfile, 80), MAT_PLATE);
  bridge.name = 'bridge_ring';
  bridge.rotation.x = Math.PI / 2;
  scene.add(bridge);

  // 夹板螺丝：一圈 6 颗蓝钢 + 一字槽
  for (let i = 0; i < 6; i++) {
    const a = (i / 6) * Math.PI * 2 + 0.28;
    const r = R_PLATE * 0.95;
    const screw = new THREE.Mesh(
      new THREE.CylinderGeometry(0.26, 0.26, 0.15, 18), MAT_BLUED);
    screw.name = 'bridge_screw_' + i;
    screw.position.set(Math.cos(a) * r, Math.sin(a) * r, Z_BRIDGE + 0.24);
    scene.add(screw);
    const slot = new THREE.Mesh(new THREE.BoxGeometry(0.34, 0.06, 0.04), MAT_STEEL);
    slot.name = 'bridge_screw_slot_' + i;
    slot.position.copy(screw.position);
    slot.position.z += 0.082;
    slot.rotation.z = a;
    scene.add(slot);
  }

  // 红宝石轴眼：每个主齿轮中心一颗（宝石轴承）
  const jewels = [
    [center.mesh.position.x, center.mesh.position.y],
    [third.mesh.position.x, third.mesh.position.y],
    [fourth.mesh.position.x, fourth.mesh.position.y]
  ];
  jewels.forEach(([jx, jy], i) => {
    const jewel = new THREE.Mesh(
      new THREE.CylinderGeometry(0.155, 0.155, 0.07, 16), MAT_JEWEL);
    jewel.name = 'jewel_' + i;
    // 宝石在齿轮上方（夹板挡住前要先看得见）——放在轮系层之上一档
    jewel.position.set(jx, jy, Z_GEAR + 0.30);
    scene.add(jewel);
  });
}

// ============================================================================
// 7. 指针 —— 父子层级 + 各自自转
// ============================================================================
//
// 三根针共用一个轴心组，各自绕 Z 转。
// 转速按真实比例：时针 1 / 分针 12 / 秒针 720（这里整体放慢便于观看）。
const hands = new THREE.Group();
hands.name = 'hands';
hands.position.set(0, 0, Z_HANDS);
scene.add(hands);

// 造一根针：中间宽两头窄的多边形（柳叶针），挤出成型
function makeHand({ length, width, tail, material, name }) {
  const shape = new THREE.Shape();
  shape.moveTo(-width * 0.5, -tail);
  shape.lineTo(width * 0.5, -tail);
  shape.lineTo(width * 0.34, length * 0.66);
  shape.lineTo(0.015, length);
  shape.lineTo(-0.015, length);
  shape.lineTo(-width * 0.34, length * 0.66);
  shape.closePath();
  const geo = new THREE.ExtrudeGeometry(shape, {
    depth: 0.065, bevelEnabled: true, bevelThickness: 0.010,
    bevelSize: 0.010, bevelSegments: 1
  });
  geo.computeVertexNormals();
  const mesh = new THREE.Mesh(geo, material);
  mesh.name = name;
  return mesh;
}

// ## 起始角度：10:10（制表业的经典展示位）
//
// 不给初值时三根针都指向 12 点、完全重叠——截图里看起来像“只有一根针”。
// 换算：“12 点”对应 +Y（针沿 +Y 长出）；时针顺时针走，
// 而 three 的 rotation.z 正值是逆时针，所以取负号。
const DEG = Math.PI / 180;

const hourHand = makeHand({
  length: 9.6, width: 1.10, tail: 1.6, material: MAT_HAND, name: 'hour_hand'
});
// 10:10 → 10.1667 时 → 顺时针 305°
hourHand.rotation.z = -((10 + 10 / 60) / 12) * 360 * DEG;
hourHand.userData.spin = { axis: [0, 0, 1], speed: -0.055 };
hands.add(hourHand);

const minuteHand = makeHand({
  length: 14.2, width: 0.82, tail: 1.9, material: MAT_HAND, name: 'minute_hand'
});
// 10 分 → 顺时针 60°
minuteHand.rotation.z = -(10 / 60) * 360 * DEG;
minuteHand.userData.spin = { axis: [0, 0, 1], speed: -0.055 * 12 };
hands.add(minuteHand);

// 秒针：极细蓝钢，长尾（手工做法）
const secondHand = makeHand({
  length: 13.6, width: 0.34, tail: 2.4, material: MAT_BLUED, name: 'second_hand'
});
// 35 秒 → 顺时针 210°
secondHand.rotation.z = -(35 / 60) * 360 * DEG;
secondHand.userData.spin = { axis: [0, 0, 1], speed: -0.055 * 720 };
hands.add(secondHand);

// 中心帽
{
  const cap = new THREE.Mesh(new THREE.CylinderGeometry(0.40, 0.50, 0.22, 20), MAT_STEEL);
  cap.name = 'hand_cap';
  cap.rotation.x = Math.PI / 2;
  cap.position.z = 0.10;
  hands.add(cap);
}

// ============================================================================
// 8. 背景 —— 一块深色圆盘（给金属一点"地面"，避免悬在虚空里）
// ============================================================================
{
  const backdrop = new THREE.Mesh(
    new THREE.CylinderGeometry(R_CASE * 1.12, R_CASE * 1.12, 0.20, 64),
    new THREE.MeshStandardMaterial({
      color: 0x141d2b, metalness: 0.45, roughness: 0.60
    }));
  backdrop.name = 'backdrop';
  backdrop.position.z = -1.55;
  backdrop.rotation.x = Math.PI / 2;
  scene.add(backdrop);
}

scene;
