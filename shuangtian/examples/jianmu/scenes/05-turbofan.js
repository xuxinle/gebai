// ============================================================================
// 涡轮风扇发动机 · 精机剖面
// ----------------------------------------------------------------------------
// 一台高涵道比涡扇的纵向剖切视图。真实涡扇的核心矛盾是「一根轴串起前后
// 两个转速完全不同的转子」——本场景把这个结构如实搭出来：
//
//   · 双转子   —— 低压转子（风扇 + 增压级 + 低压涡轮，N1）与高压转子
//                 （高压压气机 + 高压涡轮，N2）同轴套在一起，转速比 1:3.3
//   · 叶片      —— 每级沿周向均布，扭转角随级数变化（前面级大、后面级小）
//   · 燃烧室    —— 环形火焰筒 + 一圈燃油喷嘴
//   · 机匣      —— 各级直径不同（压气机逐级收小、涡轮逐级放大）
//
// ## 坐标系（这决定了整份脚本）
//
// **发动机轴沿 X**：气流从 -X 流向 +X，截面半径落在 YZ 平面内。
// 从 +Z 侧看是纵向剖面，从 -X 方向看是正对进气口。
//
//     x ≈ -11.0   进气口唇环
//     x ≈  -9.6   风扇叶片（直径最大，也是涵道分界处）
//     x ≈  -7.2   外涵道（冷气流，绕过核心机）
//     x ≈  -6.4   增压级（低压压气机，3 级）
//     x ≈  -3.6   高压压气机（5 级，直径更小、叶片更密）
//     x ≈  -1.0   燃烧室（环形，全机唯一的暖色）
//     x ≈   1.4   高压涡轮（2 级，最热）
//     x ≈   3.0   低压涡轮（4 级，逐级放大）
//     x ≈   6.0   尾锥 + 支板
//
// ## 为什么是剖切
//
// 完整的筒型机匣会把内部结构全遮住（与机械表"实心表盘挡住轮系"是同一个
// 问题）。剖切的做法：**每级机匣只画下半圈**（θ 从 0 到 π），上半圈敞开，
// 于是从侧上方能一路看进核心机。
// ============================================================================

const scene = new THREE.Scene();

// ── 轴向站位 ──────────────────────────────────────────────────────────────
const X_INTAKE = -11.2;
const X_FAN = -9.6;
const X_SPLITTER = -7.2;   // 涵道分界结束处
const X_BOOST = -6.4;      // 增压级起点
const X_HPC = -3.6;        // 高压压气机起点
const X_COMB = -1.0;       // 燃烧室
const X_HPT = 1.4;         // 高压涡轮
const X_LPT = 3.0;         // 低压涡轮
const X_PLUG = 6.0;        // 尾锥

// ── 半径 ─────────────────────────────────────────────────────────────────
const R_FAN_TIP = 9.4;     // 风扇叶尖
const R_FAN_HUB = 2.6;     // 风扇叶根
const R_NACELLE = 9.8;     // 短舱内壁
const R_SPLITTER = 4.6;    // 涵道分界（外涵道内壁 = 核心机外壁）
const R_BOOST_TIP = 4.1;
const R_HPC_TIP = 3.0;
const R_COMB_OUT = 2.8;
const R_TURB_TIP = 3.5;
const R_HUB = 1.15;        // 轮毂
const R_SHAFT = 0.40;

// ── 转速（弧度/秒）──────────────────────────────────────────────────────
// 真实涡扇 N1≈3000rpm、N2≈10000rpm，比值约 1:3.3。视觉上用慢速便于看清。
const SPIN_N1 = 0.50;
const SPIN_N2 = 1.65;

// ============================================================================
// 相机 · 布光
// ============================================================================
const camera = new THREE.PerspectiveCamera(32, 16 / 9, 1.0, 400);
// ## 视角：从侧上方看**剖切面**
//
// 剖切件只画下半圈（thetaStart=PI），所以敞开面朝上。
// 相机抬高看下去，既看到剖开的内部结构，也保留筒形体积感。
// ## 视角：接近正侧视（工程剖面图的标准看法）
//
// 发动机轴向沿 X，所以相机放在 +Z 侧看过去，整条机身在画面里是**横向**的
// —— 各级压气机/涡轮依次排开，剖切面（下半圈）朝上敞开，能一路看进核心机。
//
// 略偏 -X 与略抬高：既看出筒形体积，也不让风扇盘正对镜头变成一块灰盘
//（正对时风扇盘会占满左侧，把后面所有级都压在它后面）。
// 正侧视：相机几乎就在 +Z 侧（只抬高一点看进剖切的敞开面）
camera.position.set(-3, 13, 44);
camera.lookAt(-1.2, 0.6, 0);
scene.add(camera);

// 环境打底：金属必须有环境反射，否则一片死黑像塑料
scene.add(new THREE.AmbientLight(0x6f7f96, 0.60));

// 主光：右上前方，暖白
const keyLight = new THREE.DirectionalLight(0xfff2e0, 2.5);
keyLight.position.set(-16, 42, 32);
scene.add(keyLight);

// 补光：左后，冷色，压暗部但不抢主光
const fillLight = new THREE.DirectionalLight(0x8fb4dd, 0.80);
fillLight.position.set(28, -12, -26);
scene.add(fillLight);

// 轮廓光：后上方，把机匣边缘从背景勾出来
const rimLight = new THREE.DirectionalLight(0xdfe8ff, 1.15);
rimLight.position.set(42, 16, -18);
scene.add(rimLight);

// ============================================================================
// 材质
//
// 涡扇的颜色语言是「冷灰金属 + 一处暖色」：机匣与叶片是各级不同的钛/不锈钢，
// 燃烧室是全机唯一的高温暖色，涡轮叶片带热变色。这层区分让剖面读起来有
// 纵深，而不是一团金属。
// ============================================================================
const MAT_NACELLE = new THREE.MeshStandardMaterial({
  color: 0xa4acb6, metalness: 0.90, roughness: 0.26, side: THREE.DoubleSide,
});
const MAT_CASE = new THREE.MeshStandardMaterial({
  color: 0x89929e, metalness: 0.93, roughness: 0.34, side: THREE.DoubleSide,
});
const MAT_FAN = new THREE.MeshStandardMaterial({
  color: 0xbcc4ce, metalness: 0.97, roughness: 0.15,
});
const MAT_COMP = new THREE.MeshStandardMaterial({
  color: 0x78838f, metalness: 0.92, roughness: 0.38,
});
const MAT_COMB = new THREE.MeshStandardMaterial({
  color: 0xb07038, metalness: 0.86, roughness: 0.42, side: THREE.DoubleSide,
});
const MAT_COMB_HOT = new THREE.MeshStandardMaterial({
  color: 0xd08a45, metalness: 0.80, roughness: 0.50, side: THREE.DoubleSide,
});
const MAT_TURB = new THREE.MeshStandardMaterial({
  color: 0x9c8770, metalness: 0.90, roughness: 0.40,
});
const MAT_HOT = new THREE.MeshStandardMaterial({
  color: 0xc98a4a, metalness: 0.82, roughness: 0.46,
});
const MAT_SHAFT = new THREE.MeshStandardMaterial({
  color: 0x626b76, metalness: 0.96, roughness: 0.20,
});
const MAT_DARK = new THREE.MeshStandardMaterial({
  color: 0x363d45, metalness: 0.78, roughness: 0.56,
});

// ============================================================================
// 工具函数
//
// 发动机的一切都是**绕 X 轴**的旋转体，而 three 的 Cylinder/Lathe/Torus 都绕
// Y 轴（或法线朝 Z）。所以统一在这里做两次换算，后面所有零件都能用
// 「轴向位置 + 半径」直接描述。
// ============================================================================
const RAD = Math.PI / 180;

/// 绕 X 轴的圆筒。
///
/// `thetaStart/thetaLength` 用来只画下半圈（剖切）。注意 Cylinder 的 θ 起点在
/// +Z 方向、绕 Y 轴逆时针；转到 X 轴后对应的物理方位与直觉略有偏差——
/// 这里统一用 `openEnded=false` + 双面材质，避免靠"猜方向"来凑。
function tube(x0, x1, radius, segments, material, thetaStart, thetaLength) {
  const height = Math.abs(x1 - x0);
  // ⚠ openEnded 必须 true：false 会把圆筒两端封成**实心盘**，
  // 于是短舱变成一块盖住全部内部结构的大圆盘（与机械表"实心表盘挡住
  // 轮系"是同一个错，只是换了零件）。剖切视图里这是致命的。
  const geo = new THREE.CylinderGeometry(
    radius, radius, height, segments || 56, 1, true,
    thetaStart === undefined ? 0 : thetaStart,
    thetaLength === undefined ? Math.PI * 2 : thetaLength);
  const mesh = new THREE.Mesh(geo, material || MAT_CASE);
  mesh.position.x = (x0 + x1) * 0.5;
  mesh.rotation.z = Math.PI / 2;     // Y 轴 → X 轴
  return mesh;
}

/// 绕 X 轴的环形盘（法兰、盘体、级间封严）。
function annulus(x, rInner, rOuter, material, segments) {
  const geo = new THREE.RingGeometry(rInner, rOuter, segments || 56);
  const mesh = new THREE.Mesh(geo, material || MAT_CASE);
  mesh.position.x = x;
  mesh.rotation.y = -Math.PI / 2;    // Ring 在 XY 平面 → YZ 平面（法线朝 X）
  return mesh;
}

/// 绕 X 轴的圆盘（实心，用于轮毂端面/尾锥底）。
function plate(x, radius, material, segments) {
  const geo = new THREE.CircleGeometry(radius, segments || 56);
  const mesh = new THREE.Mesh(geo, material || MAT_CASE);
  mesh.position.x = x;
  mesh.rotation.y = -Math.PI / 2;
  return mesh;
}

// ============================================================================
// 转子组：叶片必须挂在"会转的父级"下
//
// 关键设计：每个转子建一个 `Group`，叶片全部加进组里，只给组打
// `userData.spin` —— 一圈叶片共享一次旋转。
//
// 若给每片叶子各打 spin 也对（角度相同），但那是 N 次矩阵乘法；
// 而更重要的是**语义**：同一级的叶片是刚体，属于同一个转子。
// ============================================================================
const rotorN1 = new THREE.Group();   // 低压转子：风扇 + 增压级 + 低压涡轮
rotorN1.name = 'rotor_N1';
rotorN1.userData.spin = { axis: [1, 0, 0], speed: SPIN_N1 };
scene.add(rotorN1);

const rotorN2 = new THREE.Group();   // 高压转子：高压压气机 + 高压涡轮
rotorN2.name = 'rotor_N2';
rotorN2.userData.spin = { axis: [1, 0, 0], speed: SPIN_N2 };
scene.add(rotorN2);

// ============================================================================
// 一根叶片
//
// 真实叶片的形状：根部厚、尖部薄，沿径向有**扭转**（stagger），迎风面与
// 旋转方向成一定攻角。这里用「压扁的盒子 + 逐级不同的扭转角 + 沿径向收窄」
// 近似——不是为了仿真，而是让每一级看起来确实"在推气"。
// ============================================================================
function blade(length, chordRoot, chordTip, thickness, staggerDeg, material) {
  // 叶身：一个**很薄**的长方体（length 沿局部 Y = 径向）。
  // 用两段拼出"根粗尖细"——three 的 Box 没有锥度，而顶点级改写在本框架
  // 走不通（几何是抽取后只读的），所以用接缝拼接。
  const geo = new THREE.BoxGeometry(chordRoot, length, thickness);
  const mesh = new THREE.Mesh(geo, material || MAT_COMP);
  // 尖段：更薄、弦长更短，叠在上半段
  const tip = new THREE.Mesh(
    new THREE.BoxGeometry(chordTip, length * 0.55, thickness * 0.5), material || MAT_COMP);
  tip.position.y = length * 0.22;
  mesh.add(tip);
  // 扭转（stagger）：绕径向轴 Y 转，让弦线与气流成攻角
  mesh.rotation.y = staggerDeg * RAD;
  return mesh;
}

/// 往一个组里装**一圈**叶片。
///
/// `count` 是这一级的叶片数；位置按 `2π/count` 均分，每片沿径向立起来
/// （径向 = YZ 平面内的方向，因为轴是 X）。
function bladeRing(parent, x, rHub, rTip, count, chordRoot, chordTip, thickness,
                   staggerDeg, material) {
  const length = rTip - rHub;
  const rMid = (rHub + rTip) * 0.5;
  for (let i = 0; i < count; i++) {
    const angle = (i / count) * Math.PI * 2;
    const b = blade(length, chordRoot, chordTip, thickness, staggerDeg, material);
    b.position.set(x, Math.cos(angle) * rMid, Math.sin(angle) * rMid);
    // 绕 X 转 angle，使局部 Y（叶身方向）指向该叶片所在的径向
    b.rotation.x = angle;
    parent.add(b);
  }
}

// ============================================================================
// 1. 短舱与进气口
//
// 短舱是整机最外面那层壳。剖切做法：只画下半圈，上半圈敞开。
// ============================================================================
{
  // 进气口唇环：一段短筒，直径最大
  const lip = tube(X_INTAKE, X_INTAKE + 0.9, R_NACELLE, 64, MAT_NACELLE);
  scene.add(lip);
  // 唇环前缘做成一圈小圆环（进气口圆角的视觉提示）
  const lipRing = new THREE.Mesh(
    new THREE.TorusGeometry(R_NACELLE - 0.16, 0.20, 12, 64), MAT_NACELLE);
  lipRing.position.x = X_INTAKE;
  lipRing.rotation.y = Math.PI / 2;
  scene.add(lipRing);

  // 短舱外壁（剖面：只画下半圈 → 从上方能看进去）
  scene.add(tube(X_INTAKE + 0.9, X_SPLITTER - 1.6, R_NACELLE, 64, MAT_NACELLE,
                 Math.PI, Math.PI));
  // 短舱内壁：与外壁构成壁厚。半径差给到 0.55 才读得出"蒙皮"感
  scene.add(tube(X_INTAKE + 0.9, X_SPLITTER - 1.6, R_NACELLE - 0.55, 64, MAT_CASE,
                 Math.PI, Math.PI));
  // 外涵道出口的端面（把外壁与内壁连起来，避免看见两层悬空的壳）
  scene.add(annulus(X_SPLITTER - 1.6, R_NACELLE - 0.55, R_NACELLE, MAT_NACELLE));
}

// ============================================================================
// 2. 风扇转子（低压）—— 全机直径最大的一级
// ============================================================================
{
  // 风扇盘（轮毂）
  // 风扇盘：真实宽弦扇的盘体很厚（要承受离心载荷）
  // ## 风扇盘必须是**环形**，不能是实心圆筒
    //
    // 实心筒的端面是一个直径 5.2 的圆盘，正面看会把后面整个核心机挡住
    //（与"实心表盘挡轮系"是同一个错）。真实风扇盘本来就是环形——
    // 中间穿过低压轴，气流也从不经过中心。
    const hub = tube(X_FAN - 0.85, X_FAN + 0.85, R_FAN_HUB, 48, MAT_SHAFT);
    // 中心锥（整流罩）：进气口那个子弹头
    const spinner = new THREE.Mesh(
      new THREE.ConeGeometry(R_FAN_HUB, 1.7, 40), MAT_SHAFT);
    spinner.position.x = X_FAN - 1.0;
    spinner.rotation.z = Math.PI / 2;   // 锥尖朝 -X（迎着气流）
    rotorN1.add(spinner);
  rotorN1.add(hub);

  // 风扇叶片：宽弦、大扭转、数量少（真实宽弦扇约 18~22 片）
  bladeRing(rotorN1, X_FAN, R_FAN_HUB, R_FAN_TIP, 24, 1.25, 0.78, 0.20, 38, MAT_FAN);

  // 风扇盘前后封严环
  scene.add(annulus(X_FAN - 0.55, R_FAN_HUB - 0.1, R_FAN_HUB + 0.6, MAT_CASE));
  scene.add(annulus(X_FAN + 0.55, R_FAN_HUB - 0.1, R_FAN_HUB + 0.6, MAT_CASE));
}

// ============================================================================
// 3. 增压级（低压压气机）—— 3 级，直径逐级收小
// ============================================================================
{
  const stages = 3;
  for (let i = 0; i < stages; i++) {
    const x = X_BOOST + i * 0.92;
    // 叶尖半径逐级收小（气流被压缩，通道变窄）
    const rTip = R_BOOST_TIP - i * 0.22;
    bladeRing(rotorN1, x, R_HUB, rTip, 34, 0.72, 0.52, 0.12, 38 - i * 5, MAT_COMP);
    // 级间静子（不转）：只画下半圈的一点弧段，避免糊住视线
    const stator = tube(x + 0.46, x + 0.60, rTip - 0.06, 48, MAT_DARK, Math.PI, Math.PI);
    scene.add(stator);
  }
  // 增压级轮毂（一整段）
  rotorN1.add(tube(X_BOOST - 0.4, X_BOOST + stages * 0.92, R_HUB, 48, MAT_SHAFT));
}

// ============================================================================
// 4. 高压压气机 —— 5 级，更密、直径更小、转速更高
// ============================================================================
{
  const stages = 5;
  for (let i = 0; i < stages; i++) {
    const x = X_HPC + i * 0.52;
    const rTip = R_HPC_TIP - i * 0.14;
    bladeRing(rotorN2, x, R_HUB - 0.08, rTip, 42, 0.42, 0.30, 0.075, 30 - i * 4, MAT_COMP);
    if (i < stages - 1) {
      scene.add(tube(x + 0.26, x + 0.36, rTip - 0.05, 48, MAT_DARK, Math.PI, Math.PI));
    }
  }
  rotorN2.add(tube(X_HPC - 0.3, X_HPC + stages * 0.52, R_HUB - 0.08, 48, MAT_SHAFT));
}

// ============================================================================
// 5. 燃烧室 —— 环形火焰筒（全机唯一的高温暖色）
//
// 结构：外机匣（环） + 内机匣（环） + 中间一圈燃油喷嘴。
// 这里不画"火焰"，而是用暖色金属表达高温——真实发动机在外面看是看不出火的。
// ============================================================================
{
  const x0 = X_COMB;
  const x1 = X_COMB + 2.0;

  // 外机匣（下半圈剖切）
  scene.add(tube(x0, x1, R_COMB_OUT, 56, MAT_COMB, Math.PI, Math.PI));
  // 外机匣的内壁（形成壁厚感）
  scene.add(tube(x0, x1, R_COMB_OUT - 0.30, 56, MAT_COMB_HOT, Math.PI, Math.PI));
  // 内机匣（环形火焰筒的内环）
  scene.add(tube(x0, x1, R_HUB + 0.45, 48, MAT_COMB_HOT, Math.PI, Math.PI));

  // 环形火焰筒的端面（前后封环）
  scene.add(annulus(x0, R_HUB + 0.45, R_COMB_OUT, MAT_COMB));
  scene.add(annulus(x1, R_HUB + 0.45, R_COMB_OUT, MAT_COMB));

  // 燃油喷嘴：一圈小球，沿周向均布在外机匣上
  const nozzles = 16;
  const nozzleGeo = new THREE.SphereGeometry(0.16, 12, 10);
  for (let i = 0; i < nozzles; i++) {
    const angle = (i / nozzles) * Math.PI * 2;
    const r = (R_COMB_OUT + R_HUB + 0.45) * 0.5;
    const n = new THREE.Mesh(nozzleGeo, MAT_HOT);
    n.position.set(x0 + 0.22, Math.cos(angle) * r, Math.sin(angle) * r);
    scene.add(n);
  }
}

// ============================================================================
// 6. 涡轮
//
// 高压涡轮（2 级，最热，叶片带气膜冷却孔的视觉暗示）→ 低压涡轮（4 级，
// 直径逐级放大，因为燃气在膨胀）。
// ============================================================================
{
  // 高压涡轮：两级，挂在高压转子上
  for (let i = 0; i < 2; i++) {
    const x = X_HPT + i * 0.62;
    const rTip = R_TURB_TIP - 0.55 + i * 0.10;
    bladeRing(rotorN2, x, R_HUB, rTip, 38, 0.52, 0.40, 0.11, 22, i === 0 ? MAT_HOT : MAT_TURB);
    // 导向器（静子，不转）
    scene.add(tube(x - 0.30, x - 0.20, rTip + 0.02, 48, MAT_DARK, Math.PI, Math.PI));
  }
  rotorN2.add(tube(X_HPT - 0.5, X_HPT + 0.8, R_HUB, 48, MAT_SHAFT));

  // 低压涡轮：四级，直径逐级放大（燃气膨胀）
  for (let i = 0; i < 4; i++) {
    const x = X_LPT + i * 0.66;
    const rTip = R_TURB_TIP + i * 0.16;
    bladeRing(rotorN1, x, R_HUB, rTip, 46, 0.56, 0.44, 0.10, 26 - i * 2, MAT_TURB);
    scene.add(tube(x - 0.32, x - 0.22, rTip + 0.02, 48, MAT_DARK, Math.PI, Math.PI));
  }
  rotorN1.add(tube(X_LPT - 0.5, X_LPT + 4 * 0.66, R_HUB, 48, MAT_SHAFT));
}

// ============================================================================
// 7. 机匣（核心机外壁 = 外涵道内壁）
//
// 这一层是"剖切感"的核心：它把外涵道和核心机分开，而只画下半圈之后，
// 从上方能直接看到压气机与涡轮。
// ============================================================================
{
  // 核心机外壁：从涵道分界处一直到尾喷口，半径随内部结构收放
  scene.add(tube(X_SPLITTER, X_HPC, R_SPLITTER, 64, MAT_CASE, Math.PI, Math.PI));
  scene.add(tube(X_HPC, X_COMB, R_HPC_TIP + 0.55, 56, MAT_CASE, Math.PI, Math.PI));
  scene.add(tube(X_COMB, X_HPT, R_COMB_OUT + 0.28, 56, MAT_CASE, Math.PI, Math.PI));
  scene.add(tube(X_HPT, X_LPT + 2.6, R_TURB_TIP + 0.42, 56, MAT_CASE, Math.PI, Math.PI));
}

// ============================================================================
// 8. 双转子轴 —— 两根同轴套在一起
//
// 真实结构：低压轴细长，从最前面的风扇一直通到最后面的低压涡轮；
// 高压轴是一段粗短的空心轴，只连接高压压气机与高压涡轮。
// 两轴之间靠轴承支撑——这里只画轴本身（轴承会被机匣挡住）。
// ============================================================================
{
  // N2 轴（外，粗短）：只存在于高压压气机到高压涡轮之间
  rotorN2.add(tube(X_HPC - 0.6, X_HPT + 1.0, R_SHAFT * 2.2, 32, MAT_SHAFT));
  // N1 轴（内，细长）：贯穿全机
  rotorN1.add(tube(X_FAN - 0.8, X_LPT + 2.4, R_SHAFT * 0.85, 28, MAT_SHAFT));
}

// ============================================================================
// 9. 尾锥与尾喷口
// ============================================================================
{
  // 尾锥（中心锥体，收敛形）
  const plug = new THREE.Mesh(
    new THREE.ConeGeometry(R_HUB + 0.5, 3.2, 48), MAT_CASE);
  plug.position.x = X_PLUG + 1.6;
  plug.rotation.z = -Math.PI / 2;    // 锥尖朝 +X（向后）
  scene.add(plug);

  // 尾喷口外环（收敛）
  const nozzle = new THREE.Mesh(
    new THREE.CylinderGeometry(R_TURB_TIP + 0.30, R_TURB_TIP + 0.05, 1.8, 56, 1, true),
    MAT_CASE);
  nozzle.position.x = X_PLUG + 0.9;
  nozzle.rotation.z = -Math.PI / 2;
  scene.add(nozzle);

  // 支板：沿周向几片，撑住尾锥与外环
  const struts = 6;
  for (let i = 0; i < struts; i++) {
    const angle = Math.PI + (i / struts) * Math.PI;   // 只在下半圈（剖切）
    const strut = new THREE.Mesh(new THREE.BoxGeometry(1.5, 0.12, 0.9), MAT_CASE);
    const r = (R_HUB + R_TURB_TIP) * 0.5;
    strut.position.set(X_PLUG + 0.1, Math.cos(angle) * r, Math.sin(angle) * r);
    strut.rotation.z = Math.PI / 2 - angle;
    scene.add(strut);
  }
}

// ============================================================================
// 10. 收尾
// ============================================================================
scene;
