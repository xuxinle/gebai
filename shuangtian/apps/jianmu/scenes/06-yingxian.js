// ============================================================================
// 应县木塔 · 佛宫寺释迦塔（山西应县，辽清宁二年 1056）
// ----------------------------------------------------------------------------
// 全部程序化生成，没有贴图、没有外部模型：
//
//   · 八角平面 —— 每个构件都按 8 个面的中角（i·45°）与切向排布
//   · 五层六檐 —— 五个明层各带腰檐，层间有平坐（挑出的阳台）
//   · 斗拱     —— 每层 24 攒（柱头 + 补间），逐层外挑的横拱与华拱
//   · 攒尖顶   —— 八角攒尖 + 八条垂脊，顶上接塔刹（覆钵/相轮/宝珠）
//   · 曲线举折 —— 屋面用多段折面近似凹曲线（真实屋架是"举折"而非直斜面）
//
// 尺度按实测：通高 67.31 m，底层八角直径约 30 m。本脚本 1 单位 = 1 米。
//
// ## 为什么用"合并几何"而不是每件一个 Mesh
//
// 塔上有上千个构件（椽子/瓦垄/勾栏/斗拱）。若每件一个 Mesh，物体数上千、
// 软件光栅器每帧要遍历上千次绘制调用，交互就卡死了。所以这里用一个小型的
// 几何累加器 `B`：把同材质的构件变换后累加进一条位置/法线/索引缓冲，
// 最后出**一个** Mesh。物体数因此保持在几十个量级，而细节一点不少。
// ============================================================================

const scene = new THREE.Scene();
// ## 背景与环境反射
//
// 背景选**暮色青灰**（而非纯蓝）：木构的暖橙与之对比才出“古建在黄昏里”的调子。
// 若背景太饱和的蓝，暖木会被衬成“橘黄道服”，反而显假。
scene.background = new THREE.Color(0x2a3644);
// → 本框架由 `scene.background` **推导**环境反射（上方天空 / 下方地面自动分层），
// 不存在 `environmentSky` / `environmentGround` 这类字段。
// 所以背景色同时决定了“天光有多亮”——想给金属一件更好的反射，
// 就选一个明度适中的背景，而不要指望另外调环境参数。
scene.environmentIntensity = 1.35;

// ── 尺寸约定（米）─────────────────────────────────────────────────────────
const OCT = 8;                                  // 八角
const APO = Math.cos(Math.PI / OCT);            // 中心到平面（内切圆）的比值 0.9239
const FACE = [];                                // 每个面的中角
// ## 面中角在 0°/45°/90°…，而构造八棱柱时必须补 −22.5° 的起点
//
// `CylinderGeometry` 的顶点从 `thetaStart` 开始锺，平面因此在
// `thetaStart + 22.5° + 45°k` 处。要让平面中角恰好落在 0°/45°/…，
// `thetaStart` 就得是 **−22.5°**。
//
// 两边相差一个 22.5° 的后果很阴：整个塔看起来“还行”（只是整体转了半格），
// 而只在特定位置出现的构件（台基踏步、勾栏）会直接戳到八边形轮廓外面。
for (let i = 0; i < OCT; i++) {
  const a = (i * Math.PI * 2) / OCT;
  FACE.push({ a: a, ca: Math.cos(a), sa: Math.sin(a) });
}

const STORIES = [
  // y0 = 柱脚标高，hCol = 柱高，R = 柱网外接圆半径，oh = 出檐
  { y0: 2.2, hCol: 12.0, R: 12.0, oh: 2.6 }, // 一层（一+暗层，柱最高）
  { y0: 17.0, hCol: 5.6, R: 11.0, oh: 2.5 },
  { y0: 26.2, hCol: 5.2, R: 10.2, oh: 2.4 },
  { y0: 35.0, hCol: 4.9, R: 9.4, oh: 2.4 },
  { y0: 43.4, hCol: 4.7, R: 8.6, oh: 2.3 },
];
const DOUGONG_H = 1.1; // 斗拱层高
const ROOF_RISE = 1.75; // 腰檐屋面举高（从檐口到墙根；对应约 27° 屋面坡度）
const VER_R = 13.3; // 副阶柱网半径（一层外围一圈廊柱）
const VER_H = 5.6; // 副阶柱高（低于内槽柱，才能让一层明层露出来）
const VER_DOU = 0.95; // 副阶斗拱层高
const TOP_DRIP = 49.2; // 五层檐口标高
const APEX_Y = 57.6; // 攒尖顶顶点
const SPIRE_TOP = 67.3; // 塔刹顶（通高 67.31）

// ── 材质 ──────────────────────────────────────────────────────────────────
//
// ## 为什么要分三阶木色
//
// 木构的层次全在**相邻构件的明度差**：柱最亮、枋次之、斗拱与椽头退入影里。
// 全用一个色时，远看就是一根均匀的橙柱——上千个构件的细节全被抹平。
const MAT_WOOD = new THREE.MeshStandardMaterial({
  color: 0xa8855a, metalness: 0.02, roughness: 0.82,
}); // 柱：受光主力，偏暖的旧木
const MAT_WOOD_MID = new THREE.MeshStandardMaterial({
  color: 0x86663f, metalness: 0.02, roughness: 0.86,
}); // 阑额、平坐：比柱暗一阶，拉出横向线条
const MAT_WOOD_DARK = new THREE.MeshStandardMaterial({
  color: 0x5b4128, metalness: 0.02, roughness: 0.90,
}); // 斗拱、椽头、勾栏：阴影里的深木
const MAT_TILE = new THREE.MeshStandardMaterial({
  color: 0x515a66, metalness: 0.12, roughness: 0.62,
}); // 青瓦屋面
const MAT_TILE_DARK = new THREE.MeshStandardMaterial({
  color: 0x232932, metalness: 0.06, roughness: 0.78,
}); // 檐口、垂脊
const MAT_STONE = new THREE.MeshStandardMaterial({
  color: 0x968f83, metalness: 0.0, roughness: 0.95,
}); // 台基
const MAT_SHADOW = new THREE.MeshStandardMaterial({
  color: 0x17120d, metalness: 0.0, roughness: 1.0,
  // ## 窗洞需要“透气”，不能是纯黑
  //
  // 纯黑窗洞在近距离看是**一块方黑斑**——不像室内，只像“模型漏了个洞”。
  // 给极少量自发光（室内余光）既保留“深”的观感，又让它看起来是个空间。
  emissive: 0x2a2018, emissiveIntensity: 0.5,
}); // 门窗洞内腔（极弱余光）
const MAT_GOLD = new THREE.MeshStandardMaterial({
  color: 0xc9a24a, metalness: 0.88, roughness: 0.32,
}); // 塔刹（鎏金）

// ── 几何累加器 ────────────────────────────────────────────────────────────
//
// 把任意 BufferGeometry 按矩阵变换后累加进同一份缓冲。假定矩阵只含
// 旋转 + 平移（几何尺寸直接写进 BoxGeometry 的参数），于是法线只需
// 用 3×3 部分旋转再归一化——不需要逆转置矩阵。
const B = {
  pos: [],
  nrm: [],
  idx: [],
  count: 0,

  add(geo, m) {
    const e = m ? m.elements : [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    const p = geo.attributes.position.array;
    const n = geo.attributes.normal ? geo.attributes.normal.array : null;
    const base = B.count;
    for (let i = 0; i < p.length; i += 3) {
      const x = p[i];
      const y = p[i + 1];
      const z = p[i + 2];
      B.pos.push(
        e[0] * x + e[4] * y + e[8] * z + e[12],
        e[1] * x + e[5] * y + e[9] * z + e[13],
        e[2] * x + e[6] * y + e[10] * z + e[14]
      );
      if (n) {
        const nx = e[0] * n[i] + e[4] * n[i + 1] + e[8] * n[i + 2];
        const ny = e[1] * n[i] + e[5] * n[i + 1] + e[9] * n[i + 2];
        const nz = e[2] * n[i] + e[6] * n[i + 1] + e[10] * n[i + 2];
        const len = Math.sqrt(nx * nx + ny * ny + nz * nz) || 1;
        B.nrm.push(nx / len, ny / len, nz / len);
      } else {
        B.nrm.push(0, 1, 0);
      }
    }
    B.count += p.length / 3;
    if (geo.index) {
      const ix = geo.index.array;
      for (let i = 0; i < ix.length; i++) B.idx.push(ix[i] + base);
    } else {
      for (let i = 0; i < p.length / 3; i++) B.idx.push(i + base);
    }
  },

  flush(material, name) {
    if (B.pos.length === 0) return null;
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(new Float32Array(B.pos), 3));
    g.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(B.nrm), 3));
    g.setIndex(new THREE.BufferAttribute(new Uint32Array(B.idx), 1));
    B.pos = [];
    B.nrm = [];
    B.idx = [];
    B.count = 0;
    const mesh = new THREE.Mesh(g, material);
    mesh.name = name;
    return mesh;
  },
};

// ── 变换辅助 ──────────────────────────────────────────────────────────────
const _v1 = new THREE.Vector3();
const _v2 = new THREE.Vector3();
const _v3 = new THREE.Vector3();

/** 纯平移。 */
function T(x, y, z) {
  const m = new THREE.Matrix4();
  m.setPosition(x, y, z);
  return m;
}

/** 绕 Y 旋转 ry 后平移到 (x,y,z)。ry = π/2 − a 使局部 +Z 指向该面的外法线。 */
function TR(x, y, z, ry) {
  const m = new THREE.Matrix4();
  m.makeRotationY(ry);
  m.setPosition(x, y, z);
  return m;
}

/** 从 p0 到 p1 搭一根方料：局部 +Y 沿杆轴，截面 w×d。 */
function strut(p0, p1, w, d, extraRy) {
  const dx = p1[0] - p0[0];
  const dy = p1[1] - p0[1];
  const dz = p1[2] - p0[2];
  const L = Math.sqrt(dx * dx + dy * dy + dz * dz);
  if (L < 1e-5) return;
  _v1.set(dx / L, dy / L, dz / L); // yAxis
  _v2.set(0, 1, 0);
  if (Math.abs(_v1.y) > 0.95) _v2.set(1, 0, 0);
  _v3.copy(_v2).cross(_v1).normalize(); // xAxis
  const za = _v1.clone().cross(_v3).normalize(); // zAxis
  const m = new THREE.Matrix4();
  m.makeBasis(_v3, _v1, za);
  m.setPosition((p0[0] + p1[0]) * 0.5, (p0[1] + p1[1]) * 0.5, (p0[2] + p1[2]) * 0.5);
  if (extraRy) {
    const r = new THREE.Matrix4().makeRotationY(extraRy);
    m.multiply(r);
  }
  B.add(new THREE.BoxGeometry(w, L, d), m);
}

/** 八角柱/台（半径 rt 顶、rb 底，高 h，中心标高 cy）。面心对齐 i·45°。 */
function octPrism(rt, rb, h, cy) {
  // thetaStart = −22.5°：让平面中角落在 0°/45°/…，与 FACE 同相位
  const g = new THREE.CylinderGeometry(rt, rb, h, OCT, 1, false, -Math.PI / OCT);
  B.add(g, T(0, cy, 0));
}

// ============================================================================
// 一、台基（两层须弥座式石台）
// ============================================================================
{
  // ## 台基要**埋进地面**，不要坐在离地 0.005 的悬空面上
  //
  // 底面与地面只差 0.015 时，两者在远处就会深度打架（抖动成碎片）。
  // 把台基往下延到 y=-0.45（地表在 -0.25），两张面相交而不是相邻，
  // 这类抖动就一律消失。
  octPrism(15.6, 16.0, 1.60, 0.36); // 下枋（底 -0.44）
  octPrism(14.8, 15.2, 0.45, 1.38); // 束腰
  octPrism(14.2, 14.6, 0.60, 1.90); // 上枋（台面 2.20）
  const m = B.flush(MAT_STONE, 'platform');
  if (m) scene.add(m);

  // 正面踏道（石阶）：只做四个正向面，其余面是台基侧壁
  // ## 每级踏步必须是“从地面砌到踏面”的实心块
  //
  // 第一版写成“每级一个 0.4 厚的薄板，摆在不同标高”：板与板之间留着空隙，
  // 远看就是一排悬空的铁栅栏，不是台阶。正确做法：第 s 级的块高 = 它的踏面高，
  // 底边落地——这样叠出来才是真台阶。
  for (let i = 0; i < OCT; i += 2) {
    const f = FACE[i];
    const ry = Math.PI / 2 - f.a;
    const steps = 5;
    const rise = 0.44; // 台面 2.20 / 5
    const tread = 0.52;
    // ## 踏步的内沿必须从**台基平面**算起（不是从外接圆半径）
    //
    // `octPrism` 的半径是外接圆，平面对中心的距离是 r×APO（= 0.924r）。
    // 把踏步按裸半径 16.0 排，它就会在台基面（14.78）之外悬空 1.2——
    // 看上去是两根孤立的白色细条，而不是台阶。
    const rBase = 15.6 * APO + 0.06; // 台基下枋的平面位置
    for (let s = 0; s < steps; s++) {
      const top = rise * (s + 1);
      const r = rBase + tread * (steps - s);
      // 底边埋到地面以下，与台基同一个做法（避开悬空面的深度抖动）
      B.add(
        new THREE.BoxGeometry(5.6, top + 0.45, tread),
        TR(f.ca * (r - tread * 0.5), (top - 0.45) * 0.5, f.sa * (r - tread * 0.5), ry)
      );
    }
  }
  const st = B.flush(MAT_STONE, 'platform_steps');
  if (st) scene.add(st);
}

// ============================================================================
// 二、副阶周匝（一层外围的一圈廊柱 + 独立屋檐：这就是“五层六檐”的第六檐）
// ============================================================================
{
  const RV = VER_R;
  const yBase = 2.2;
  const faceW = 2 * RV * Math.sin(Math.PI / OCT);

  // 廊柱（每面 2 根，比内槽略细）
  for (let i = 0; i < OCT; i++) {
    const f = FACE[i];
    for (let k = -1; k <= 1; k += 2) {
      const off = k * faceW * 0.30;
      B.add(
        new THREE.CylinderGeometry(0.27, 0.31, VER_H, 10),
        T(f.ca * RV * APO - f.sa * off, yBase + VER_H * 0.5, f.sa * RV * APO + f.ca * off)
      );
    }
  }
  const vc = B.flush(MAT_WOOD, 'veranda_columns');
  if (vc) scene.add(vc);

  // 阑额（廊柱头一道横枋）
  octPrism(RV * APO + 0.05, RV * APO + 0.05, 0.48, yBase + VER_H - 0.24);
  const va = B.flush(MAT_WOOD_MID, 'veranda_architrave');
  if (va) scene.add(va);

  // 副阶斗拱（简化：每面一攒一层的三层叠斗）
  {
    const yD = yBase + VER_H;
    const rB = RV * APO - 0.1;
    for (let i = 0; i < OCT; i++) {
      const f = FACE[i];
      const ry = Math.PI / 2 - f.a;
      for (let k = -1; k <= 1; k += 2) {
        const off = k * faceW * 0.30;
        const cx = f.ca * rB - f.sa * off;
        const cz = f.sa * rB + f.ca * off;
        const put = (w, h, d, dr, dy) => {
          B.add(
            new THREE.BoxGeometry(w, h, d),
            TR(cx + f.ca * dr, yD + dy, cz + f.sa * dr, ry)
          );
        };
        put(0.62, 0.30, 0.62, 0.0, 0.15);
        put(1.30, 0.24, 0.30, 0.10, 0.40);
        put(0.30, 0.24, 0.95, 0.40, 0.40);
        put(0.28, 0.22, 1.40, 0.82, 0.62);
        put(1.90, 0.22, 0.30, 1.15, 0.78);
      }
    }
    octPrism(RV * APO - 0.55, RV * APO - 0.55, VER_DOU, yD + VER_DOU * 0.5);
    const vd = B.flush(MAT_WOOD_DARK, 'veranda_dougong');
    if (vd) scene.add(vd);
  }

  // 副阶屋面（从檐口向内抬到一层墙根）
  {
    const yDrip = yBase + VER_H + VER_DOU;
    const rDrip = RV * APO + 2.8;
    const rWall = 12.0 * APO + 0.3;
    const rise = 2.45;
    // 与腰檐同一套举折语言（段数 8 / 指数 2.1），否则副阶与各层屋面“弯得不一样”
    const segs = 8;
    const kRise = 2.1;
    for (let s = 0; s < segs; s++) {
      const t0 = s / segs;
      const t1 = (s + 1) / segs;
      const r0 = rDrip + (rWall - rDrip) * t0;
      const r1 = rDrip + (rWall - rDrip) * t1;
      const b0 = yDrip + rise * Math.pow(t0, kRise);
      const b1 = yDrip + rise * Math.pow(t1, kRise);
      octPrism(r1, r0, b1 - b0, (b0 + b1) * 0.5);
    }
    const vr = B.flush(MAT_TILE, 'veranda_roof');
    if (vr) scene.add(vr);

    // 檐口厚边 + 椽头
    octPrism(rDrip + 0.06, rDrip + 0.06, 0.20, yDrip - 0.04);
    octPrism(rDrip - 0.10, rDrip - 0.10, 0.22, yDrip - 0.25);
    const ve = B.flush(MAT_TILE_DARK, 'veranda_eave');
    if (ve) scene.add(ve);

    {
      const n = 26;
      const rEnd = rDrip - 0.42;
      for (let i = 0; i < OCT; i++) {
        const f = FACE[i];
        const pX = -f.sa;
        const pZ = f.ca;
        for (let k = 0; k < n; k++) {
          const off = ((k + 0.5) / n - 0.5) * faceW * 1.05;
          B.add(
            new THREE.BoxGeometry(0.17, 0.19, 0.46),
            TR(f.ca * rEnd + pX * off, yDrip - 0.30, f.sa * rEnd + pZ * off,
               Math.PI / 2 - f.a)
          );
        }
      }
      const vraf = B.flush(MAT_WOOD, 'veranda_rafters');
      if (vraf) scene.add(vraf);
    }

    // 瓦垄 + 翼角起翘 + 风铎
    {
      const ridges = 12;
      for (let i = 0; i < OCT; i++) {
        const f = FACE[i];
        const pX = -f.sa;
        const pZ = f.ca;
        for (let k = 0; k < ridges; k++) {
          const off = ((k + 0.5) / ridges - 0.5) * faceW * 0.97;
          strut(
            [f.ca * (rDrip - 0.10) + pX * off, yDrip + 0.08, f.sa * (rDrip - 0.10) + pZ * off],
            [f.ca * (rWall + 0.15) + pX * off, yDrip + rise + 0.02,
             f.sa * (rWall + 0.15) + pZ * off],
            0.085, 0.05
          );
        }
      }
      for (let i = 0; i < OCT; i++) {
        const ac = Math.PI / OCT + (i * Math.PI * 2) / OCT;
        // 翼角起翘：起点在檐口、往上向内收。
        // 旧写法抬 0.72、外伸 0.95 —— 绠点悬在屋面之上方很多，读起来像黑棒；
        // 改成抬 0.50、外伸 0.80，绠点刚好在屋面延长线上，才是“翘”不是“杆”。
        strut(
          [Math.cos(ac) * rDrip, yDrip + 0.04, Math.sin(ac) * rDrip],
          [Math.cos(ac) * (rDrip + 0.80), yDrip + 0.50, Math.sin(ac) * (rDrip + 0.80)],
          0.26, 0.22
        );
        const bell = new THREE.ConeGeometry(0.14, 0.34, 6);
        const bm = new THREE.Matrix4().makeRotationX(Math.PI);
        bm.setPosition(Math.cos(ac) * (rDrip + 0.80), yDrip - 0.26,
                       Math.sin(ac) * (rDrip + 0.80));
        B.add(bell, bm);
      }
      const vri = B.flush(MAT_TILE_DARK, 'veranda_ridges');
      if (vri) scene.add(vri);
    }
  }

  // 副阶地面（廊下的石地面，与外侧台基同一个高度）
  octPrism(RV * APO + 1.2, RV * APO + 1.2, 0.22, yBase - 0.11);
  const vfl = B.flush(MAT_STONE, 'veranda_floor');
  if (vfl) scene.add(vfl);
}

// ============================================================================
// 三、五层塔身
// ============================================================================
for (let si = 0; si < STORIES.length; si++) {
  const S = STORIES[si];
  const y0 = S.y0;
  const R = S.R;
  const yArch = y0 + S.hCol; // 柱头 / 额枋顶
  const yDou = yArch; // 斗拱层起点
  const yDrip = yDou + DOUGONG_H; // 檐口标高
  const cy = y0 + S.hCol * 0.5;

  // ── 2.1 平坐（二层及以上：挑出的阳台板 + 勾栏）──
  // 一层没有平坐（它在台基上），二~五层每层柱脚处有一圈挑台。
  if (si > 0) {
    const pR = R + 1.7;
    octPrism(pR, pR, 0.36, y0 - 0.18); // 挑台板
    octPrism(pR, pR - 0.5, 0.34, y0 - 0.62); // 板底加劲（收进去，像斗拱挑出）
    const m = B.flush(MAT_WOOD_MID, 'balcony_' + (si + 1));
    if (m) scene.add(m);

    // 勾栏：望柱 + 上下枋 + 蜀柱
    const rR = pR - 0.28;
    for (let i = 0; i < OCT; i++) {
      const a0 = (i * Math.PI * 2) / OCT;
      const a1 = ((i + 1) * Math.PI * 2) / OCT;
      const p0 = [Math.cos(a0) * rR * APO, y0, Math.sin(a0) * rR * APO];
      const p1 = [Math.cos(a1) * rR * APO, y0, Math.sin(a1) * rR * APO];
      // 上下两道横枋
      strut(p0, p1, 0.10, 0.14);
      strut(
        [p0[0], p0[1] + 0.95, p0[2]],
        [p1[0], p1[1] + 0.95, p1[2]],
        0.13, 0.17
      );
      // 蜀柱（每边 5 根）
      for (let k = 1; k <= 5; k++) {
        const t = k / 6;
        const x = p0[0] + (p1[0] - p0[0]) * t;
        const z = p0[2] + (p1[2] - p0[2]) * t;
        B.add(new THREE.BoxGeometry(0.13, 1.02, 0.13), T(x, y0 + 0.5, z));
      }
    }
    const rl = B.flush(MAT_WOOD_DARK, 'railing_' + (si + 1));
    if (rl) scene.add(rl);
  }

  // ── 2.2 柱网（每面 3 根，共 24 根）──
  //
  // 柱头 / 柱础是“近距离精致”的关键：光杆圆柱一看就是程序几何，
  // 而真实的木柱两端都有交代（下坐石础、上顶阑额）。这里用两个方块/倒锤形解决。
  const faceW = 2 * R * Math.sin(Math.PI / OCT);
  for (let i = 0; i < OCT; i++) {
    const f = FACE[i];
    for (let k = -1; k <= 1; k++) {
      const off = k * faceW * 0.31;
      const x = f.ca * R * APO - f.sa * off;
      const z = f.sa * R * APO + f.ca * off;
      B.add(
        new THREE.CylinderGeometry(0.30, 0.34, S.hCol, 10),
        T(x, y0 + S.hCol * 0.5, z)
      );
      // 柱头：椒斗形（上宽下窄），与斗拱层衔接
      B.add(
        new THREE.CylinderGeometry(0.42, 0.30, 0.26, OCT),
        T(x, y0 + S.hCol - 0.13, z)
      );
      // 柱础：方形石础（**石材**，不是木色——石础与木柱的色差本身就是一道层次）
      B.add(
        new THREE.CylinderGeometry(0.36, 0.38, 0.30, OCT),
        T(x, y0 + 0.15, z)
      );
    }
  }
  const cols = B.flush(MAT_WOOD, 'columns_' + (si + 1));
  if (cols) scene.add(cols);

  // ── 2.3 墙身 / 门窗 ──
  {
    const rFace = R * APO; // 平面到中心的距离
    const wallT = 0.55;
    if (si === 0) {
      // 一层：厚墙 + 四面板门（门是真缺口，能看见里面的暗腔）
      const doorW = 4.4;
      const doorH = 6.2;
      for (let i = 0; i < OCT; i++) {
        const f = FACE[i];
        const ry = Math.PI / 2 - f.a;
        const hasDoor = i % 2 === 0;
        if (!hasDoor) {
          B.add(
            new THREE.BoxGeometry(faceW + 0.5, S.hCol, wallT),
            TR(f.ca * (rFace - wallT * 0.5), cy, f.sa * (rFace - wallT * 0.5), ry)
          );
        } else {
          const pierW = (faceW + 0.5 - doorW) * 0.5;
          for (const sgn of [-1, 1]) {
            const off = sgn * (doorW + pierW) * 0.5;
            B.add(
              new THREE.BoxGeometry(pierW, S.hCol, wallT),
              TR(
                f.ca * (rFace - wallT * 0.5) - f.sa * off,
                cy,
                f.sa * (rFace - wallT * 0.5) + f.ca * off,
                ry
              )
            );
          }
          // 门楣以上
          B.add(
            new THREE.BoxGeometry(doorW, S.hCol - doorH, wallT),
            TR(f.ca * (rFace - wallT * 0.5), y0 + doorH + (S.hCol - doorH) * 0.5,
               f.sa * (rFace - wallT * 0.5), ry)
          );
          // 门扇（略退进，深色）
          B.add(
            new THREE.BoxGeometry(doorW - 0.3, doorH - 0.1, 0.16),
            TR(f.ca * (rFace - wallT - 0.1), y0 + doorH * 0.5,
               f.sa * (rFace - wallT - 0.1), ry)
          );
        }
      }
      const w = B.flush(MAT_WOOD_DARK, 'wall_1');
      if (w) scene.add(w);
    } else {
      // 二~五层：每面一樘直棂窗（窗下砌槛墙）
      const winW = faceW * 0.52;
      const sillH = 0.85;
      const winH = S.hCol * 0.52;
      for (let i = 0; i < OCT; i++) {
        const f = FACE[i];
        const ry = Math.PI / 2 - f.a;
        // 槛墙（窗下）+ 窗上墙
        B.add(
          new THREE.BoxGeometry(faceW + 0.4, sillH, wallT),
          TR(f.ca * (rFace - wallT * 0.5), y0 + sillH * 0.5,
             f.sa * (rFace - wallT * 0.5), ry)
        );
        B.add(
          new THREE.BoxGeometry(faceW + 0.4, S.hCol - sillH - winH, wallT),
          TR(f.ca * (rFace - wallT * 0.5), y0 + sillH + winH + (S.hCol - sillH - winH) * 0.5,
             f.sa * (rFace - wallT * 0.5), ry)
        );
        // 窗两侧墙垛
        const sideW = (faceW + 0.4 - winW) * 0.5;
        for (const sgn of [-1, 1]) {
          const off = sgn * (winW + sideW) * 0.5;
          B.add(
            new THREE.BoxGeometry(sideW, winH, wallT),
            TR(
              f.ca * (rFace - wallT * 0.5) - f.sa * off,
              y0 + sillH + winH * 0.5,
              f.sa * (rFace - wallT * 0.5) + f.ca * off,
              ry
            )
          );
        }
      }
      const w = B.flush(MAT_WOOD_DARK, 'wall_' + (si + 1));
      if (w) scene.add(w);

      // 窗芯（暗）+ 直棂（竖向细木条）
      for (let i = 0; i < OCT; i++) {
        const f = FACE[i];
        const ry = Math.PI / 2 - f.a;
        B.add(
          new THREE.BoxGeometry(winW - 0.1, winH - 0.1, 0.10),
          TR(f.ca * (rFace - wallT - 0.06), y0 + sillH + winH * 0.5,
             f.sa * (rFace - wallT - 0.06), ry)
        );
      }
      const g = B.flush(MAT_SHADOW, 'window_sash_' + (si + 1));
      if (g) scene.add(g);

      for (let i = 0; i < OCT; i++) {
        const f = FACE[i];
        const ry = Math.PI / 2 - f.a;
        const bars = 7;
        for (let k = 0; k < bars; k++) {
          const off = (k / (bars - 1) - 0.5) * (winW - 0.55);
          B.add(
            new THREE.BoxGeometry(0.075, winH - 0.16, 0.10),
            TR(
              f.ca * (rFace - wallT - 0.14) - f.sa * off,
              y0 + sillH + winH * 0.5,
              f.sa * (rFace - wallT - 0.14) + f.ca * off,
              ry
            )
          );
        }
      }
      const b = B.flush(MAT_WOOD, 'window_bars_' + (si + 1));
      if (b) scene.add(b);
    }

    // 内腔（让门洞看进去是深色而不是穿帮到背面）
    octPrism(R * APO - 1.2, R * APO - 1.2, S.hCol, cy);
    const core = B.flush(MAT_SHADOW, 'core_' + (si + 1));
    if (core) scene.add(core);
  }

  // ── 2.4 额枋（柱头一圈横枋）──
  octPrism(R * APO + 0.06, R * APO + 0.06, 0.55, yArch - 0.28);
  {
    const m = B.flush(MAT_WOOD_MID, 'architrave_' + (si + 1));
    if (m) scene.add(m);
  }

  // ── 2.5 斗拱层（每层 24 攒：柱头 24 + 补间，简化为每面 3 攒）──
  {
    const rBase = R * APO - 0.15;
    const sc = 1.0 - si * 0.05;
    for (let i = 0; i < OCT; i++) {
      const f = FACE[i];
      const ry = Math.PI / 2 - f.a;
      for (let k = -1; k <= 1; k++) {
        const off = k * faceW * 0.31;
        const cx = f.ca * rBase - f.sa * off;
        const cz = f.sa * rBase + f.ca * off;
        const put = (w, h, d, dr, dy) => {
          B.add(
            new THREE.BoxGeometry(w, h, d),
            TR(cx + f.ca * dr, yDou + dy, cz + f.sa * dr, ry)
          );
        };
        // 坐斗（大斗）
        put(0.74 * sc, 0.34, 0.74 * sc, 0.0, 0.17);
        // 泥道拱（顺身，沿切向）
        put(1.55 * sc, 0.26, 0.32, 0.10, 0.47);
        // 华拱一跳（径向伸出）
        put(0.34, 0.26, 1.05, 0.42, 0.47);
        // 慢拱（第二层顺身，更长）
        put(1.95 * sc, 0.24, 0.30, 0.72, 0.70);
        // 华拱二跳
        put(0.32, 0.24, 1.55, 0.95, 0.70);
        // 令拱（最外一跳，承托檐口）
        put(2.35 * sc, 0.26, 0.34, 1.42, 0.94);
        // 散斗（令拱上的小斗，**两个**——只放一个居中会像一颗钉）
        //
        // 实测：单个居中散斗（0.30 立方）+ 深色，远看就是檐下一排黑钉子。
        // 拆成两个、横置且略宽，才读得出“斗”而不“钉”。
        put(0.34, 0.20, 0.30, 1.30, 1.08);
        put(0.34, 0.20, 0.30, 1.54, 1.08);
      }
    }
    // ## 斗拱用**中间阶**而不是最暗阶
    //
    // 斗拱坐在檐下阴影里，本身已经很少受光。若再给它最暗的木色，
    // 远看就是一条黑带——几何做得再细也自废。中色调刚好能让
    // 每一跳华拱的挑出轮廓浮出来（“层层叠叠”的观感就是靠这个）。
    const m = B.flush(MAT_WOOD_MID, 'dougong_' + (si + 1));
    if (m) scene.add(m);

    // 斗拱层背后的垫板（遮住梁架间的空隙）
    octPrism(R * APO - 0.55, R * APO - 0.55, DOUGONG_H, yDou + DOUGONG_H * 0.5);
    const pad = B.flush(MAT_WOOD_DARK, 'dougong_pad_' + (si + 1));
    if (pad) scene.add(pad);
  }

  // ── 2.6 腰檐（**前四层**）──
  //
  // 第五层的檐就是塔顶攒尖顶的檐，不另做一层——这正是“五层六檐”的来历：
  // 底层副阶一檐 + 五层各自的主檐（第五层并给攒尖顶）= 五层而六檐。
  // 两处都做会多盖出一圈屋面，视觉上第五层就变成“两层”了。
  //
  // 屋面用 4 段折面近似"举折"凹曲线：靠墙平缓、近檐口陡峻（真实中国屋顶
  // 是上凹的，直斜面会显得呆板）。檐口另有一圈厚檐板（瓦口）。
  if (si < STORIES.length - 1) {
    const rWall = R * APO + 0.30;
    const rDrip = R * APO + S.oh;
    // ## 举折：段数与曲率
    //
    // 古建屋面不是直斜面：从檐口往里先**平缓**，靠近脊部再**陡起**（凹曲线）。
    // 段数太少会让它退化成直斜面（看上去呆板、像现代坡屋顶）；
    // 指数决定“弯在哪里”：`pow(t,1.55)` 的弯曲靠近檐口，视觉上几乎看不出凹，
    // 改用更大的指数与更多段，弯曲集中在近脊一半——那才是举折的样子。
    const segs = 8;
    const kRise = 2.1;
    for (let s = 0; s < segs; s++) {
      const t0 = s / segs;
      const t1 = (s + 1) / segs;
      // 半径从檐口向内收；标高从檐口向墙根抬（凹曲线）
      const r0 = rDrip + (rWall - rDrip) * t0;
      const r1 = rDrip + (rWall - rDrip) * t1;
      const yOff = ROOF_RISE * Math.pow(t1, kRise);
      const yOff0 = ROOF_RISE * Math.pow(t0, kRise);
      const h = yOff - yOff0;
      if (h < 0.02) continue;
      octPrism(r1, r0, h, yDrip + (yOff0 + yOff) * 0.5);
    }
    const roof = B.flush(MAT_TILE, 'eave_' + (si + 1));
    if (roof) scene.add(roof);

    // 檐口瓦口（只给一适厚边，不要太厚——太厚就成了“帽檐”）
    octPrism(rDrip + 0.06, rDrip + 0.06, 0.20, yDrip - 0.04);
    const edge = B.flush(MAT_TILE_DARK, 'eave_edge_' + (si + 1));
    if (edge) scene.add(edge);

    octPrism(rDrip - 0.10, rDrip - 0.10, 0.22, yDrip - 0.25);
    const soffit = B.flush(MAT_WOOD_DARK, 'soffit_' + (si + 1));
    if (soffit) scene.add(soffit);

    // 椽头（檐下一排外露的椽木端头）——中国木构“檐下阴影里那排白点”的来历，
    // 没有它屋面就只是一块贴上去的板。
    {
      const n = 26;
      const rEnd = rDrip - 0.42;
      for (let i = 0; i < OCT; i++) {
        const f = FACE[i];
        const pX = -f.sa;
        const pZ = f.ca;
        for (let k = 0; k < n; k++) {
          const off = ((k + 0.5) / n - 0.5) * faceW * 1.0;
          B.add(
            new THREE.BoxGeometry(0.17, 0.19, 0.46),
            TR(
              f.ca * rEnd + pX * off * (rEnd / (R * APO)),
              yDrip - 0.30,
              f.sa * rEnd + pZ * off * (rEnd / (R * APO)),
              Math.PI / 2 - f.a
            )
          );
        }
      }
      const raft = B.flush(MAT_WOOD, 'rafter_ends_' + (si + 1));
      if (raft) scene.add(raft);
    }

    // 瓦垄（沿坡向的细棱，程序化出一片片的瓦条感）
    const ridges = 14; // 每面瓦垄数
    for (let i = 0; i < OCT; i++) {
      const f = FACE[i];
      const perpX = -f.sa;
      const perpZ = f.ca;
      for (let k = 0; k < ridges; k++) {
        const off = ((k + 0.5) / ridges - 0.5) * faceW * 0.97;
        const p0 = [
          f.ca * (rDrip - 0.10) + perpX * off * (rDrip / (R * APO)),
          yDrip + 0.08,
          f.sa * (rDrip - 0.10) + perpZ * off * (rDrip / (R * APO)),
        ];
        const p1 = [
          f.ca * (rWall + 0.15) + perpX * off * (rWall / (R * APO)),
          yDrip + ROOF_RISE + 0.02,
          f.sa * (rWall + 0.15) + perpZ * off * (rWall / (R * APO)),
        ];
        strut(p0, p1, 0.085, 0.05);
      }
    }
    const wr = B.flush(MAT_TILE_DARK, 'tile_ridges_' + (si + 1));
    if (wr) scene.add(wr);

    // 翼角起翘 + 风铎（八角各一）
    for (let i = 0; i < OCT; i++) {
      const ac = Math.PI / OCT + (i * Math.PI * 2) / OCT; // 角部方向
      const cxp = Math.cos(ac) * rDrip;
      const czp = Math.sin(ac) * rDrip;
      // 起翘：从檐口角点向外上方挑出一小段
      //
      // 抬 0.72 / 外伸 0.95 时，绠点明显高于屋面延长线，
      // 远看就是“檐角插了一圈黑签”。改成 0.50 / 0.80，让它贴着屋面走向。
      strut(
        [cxp, yDrip + 0.04, czp],
        [Math.cos(ac) * (rDrip + 0.80), yDrip + 0.50, Math.sin(ac) * (rDrip + 0.80)],
        0.26, 0.22
      );
      // 风铎：小锥体垂在翘角下
      const bell = new THREE.ConeGeometry(0.14, 0.34, 6);
      const bm = new THREE.Matrix4().makeRotationX(Math.PI);
      bm.setPosition(
        Math.cos(ac) * (rDrip + 0.80),
        yDrip - 0.26,
        Math.sin(ac) * (rDrip + 0.80)
      );
      B.add(bell, bm);
    }
    const tip = B.flush(MAT_TILE_DARK, 'corner_tips_' + (si + 1));
    if (tip) scene.add(tip);
  }
}

// ============================================================================
// 四、攒尖顶（八角锥形屋面 + 八条垂脊）
// ============================================================================
{
  const rDrip = 8.6 * APO + 2.3;
  const rTop = 0.75;
  const segs = 12;
  for (let s = 0; s < segs; s++) {
    const t0 = s / segs;
    const t1 = (s + 1) / segs;
    const r0 = rDrip + (rTop - rDrip) * t0;
    const r1 = rDrip + (rTop - rDrip) * t1;
    // 凹曲线：愈上愈陡（真实举折）。攒尖顶是八角锥，斜率变化比腰檐更明显，
    // 指数与段数都比腰檐再大一些，否则锥面看上去就是直棱锥。
    const base0 = TOP_DRIP + (APEX_Y - TOP_DRIP) * Math.pow(t0, 1.85);
    const base1 = TOP_DRIP + (APEX_Y - TOP_DRIP) * Math.pow(t1, 1.85);
    octPrism(r1, r0, base1 - base0, (base0 + base1) * 0.5);
  }
  const roof = B.flush(MAT_TILE, 'top_roof');
  if (roof) scene.add(roof);

  // 檐口厚边
  octPrism(rDrip + 0.08, rDrip + 0.08, 0.24, TOP_DRIP - 0.05);
  const edge = B.flush(MAT_TILE_DARK, 'top_eave_edge');
  if (edge) scene.add(edge);

  // 八条垂脊：从檐口角点沿**屋面爬升**到顶点
  for (let i = 0; i < OCT; i++) {
    const ac = Math.PI / OCT + (i * Math.PI * 2) / OCT;
    const p0 = [Math.cos(ac) * rDrip, TOP_DRIP + 0.12, Math.sin(ac) * rDrip];
    const p1 = [0, APEX_Y - 0.1, 0];
    strut(p0, p1, 0.34, 0.26);
    // 脊端起翘（翼角）：
    //
    // 它必须**贴着屋面走向**往上翊，不能凭空向外伸——旧写法从檐口向
    // 外上方拉一根 1.15 长的斜杆，而那个位置屋面已经往下走了，
    // 于是它整根悬在屋面外，远看就是“八角上插了一圈黑棒”。
    //
    // 改成：起点略向外（挠角要出檐），但绠点仍在屋面**之上**、靠近檐脊，
    // 且抬升量小（0.55）——那样才是“翼角起翘”，而不是天线。
    strut(
      [Math.cos(ac) * (rDrip + 0.30), TOP_DRIP + 0.10, Math.sin(ac) * (rDrip + 0.30)],
      [Math.cos(ac) * (rDrip + 0.95), TOP_DRIP + 0.62, Math.sin(ac) * (rDrip + 0.95)],
      0.30, 0.22
    );
  }
  const hips = B.flush(MAT_TILE_DARK, 'top_hips');
  if (hips) scene.add(hips);
}

// ============================================================================
// 五、塔刹（覆钵 · 相轮 · 宝盖 · 宝珠）
// ============================================================================
{
  // 刹座（覆钵）+ 仰莲一层
  octPrism(2.35, 2.9, 0.9, APEX_Y + 0.45);
  B.add(new THREE.SphereGeometry(2.05, 20, 14), T(0, APEX_Y + 1.75, 0));
  octPrism(2.1, 2.35, 0.28, APEX_Y + 2.9);
  // 刹杆 + 七重相轮（紧密排列的圆环，中轴几乎看不见——
  // 相轮是“一层层套在杆上的圆盘”，不是笼子）
  B.add(new THREE.CylinderGeometry(0.46, 0.56, 8.4, 14), T(0, APEX_Y + 6.3, 0));
  for (let i = 0; i < 7; i++) {
    const y = APEX_Y + 3.35 + i * 0.82;
    const r = 2.05 - i * 0.19;
    // ## 相轮要“薄”
    //
    // 真实的相轮是**一层层薄盘**套在刹杆上，侧面看是“一排细线”，
    // 而不是叠起来的碗。旧写法盘厚 0.34（直径 4 时厚比接近 1:12，但间隙小），
    // 加上 7 层紧密排列，侧面就糊成了一根圆锥。
    //
    // 改法：盘再薄一截（0.16），间隙略拉开，另加一圈稍大的“轮沿”（矮环）——
    // 轮沿是相轮真正的识别特征（“轮”的轮廓靠它）。
    B.add(new THREE.CylinderGeometry(r, r, 0.16, 28), T(0, y, 0));
    // 轮沿：比盘略大、很矮，形成“盘 + 边”的层次
    B.add(new THREE.CylinderGeometry(r * 1.06, r * 1.06, 0.07, 28), T(0, y, 0));
    // 盘心凸起（套在刹杆上的毂）
    B.add(new THREE.CylinderGeometry(r * 0.30, r * 0.30, 0.26, 18), T(0, y, 0));
  }
  // 宝盖（伞状小檐，八棱，带垂脊感）
  octPrism(1.05, 2.15, 0.62, APEX_Y + 11.25);
  octPrism(2.15, 2.15, 0.12, APEX_Y + 10.88);
  // 宝珠（上下两颗：取宝珠与宝瓶之意）
  B.add(new THREE.SphereGeometry(0.86, 18, 12), T(0, SPIRE_TOP - 1.05, 0));
  B.add(new THREE.SphereGeometry(0.42, 14, 10), T(0, SPIRE_TOP - 0.15, 0));
  B.add(new THREE.CylinderGeometry(0.30, 0.36, 1.0, 12), T(0, SPIRE_TOP - 2.0, 0));
  const spire = B.flush(MAT_GOLD, 'spire');
  if (spire) scene.add(spire);
}

// ============================================================================
// 六、地面（不参与取景：noFrame）
// ============================================================================
{
  const ground = new THREE.Mesh(
    // 半径要大到“看不到地面边缘”（半径 150 时视线会越过柱面，塔前露出一道蓝色裂口），
    // 但一旦这么大，相机 near 就不能还停在 0.5——见下面的 near_z 注释。
    new THREE.CylinderGeometry(900, 900, 0.4, 64),
    // ## 地面不能是“纯漫反射色块”
    //
    // 本渲染器没有贴图与阴影，一块均匀漫反射地面必然是一个无纵深的色块。
    // 给它**轻微金属 + 略低粗糙度**后，地面会接收环境反射与平行光的角度变化：
    // 近处（视角大）与远处（视角缓）的亮度自然不同，纵深感就出来了。
    // 仍保留高基础粗糙度，不至于变成镜面。
    new THREE.MeshStandardMaterial({
      color: 0x585044, metalness: 0.06, roughness: 0.88,
    })
  );
  ground.name = 'ground';
  ground.position.y = -0.45;
  ground.userData.noFrame = true;
  scene.add(ground);

  // ## 不做“假投影”（试过，更糟）
  //
  // `Scene3D` 没有 shadow map，所以试过用几层同心暗盘堆出软阴影。结果失败：
  // `MeshBasicMaterial` 在本渲染器映射为“**自发光 + unlit**”，
  // 暗色叠在地面上反而**比地面更亮**，加上只有 4 个步进，
  // 画面变成一圈明显的同心光靶（反而把视线吸到地上）。
  //
  // 结论：接地感靠**台基自身的明度与梯度**做（石台已比地面亮一阶），
  // 不靠假阴影。真要阴影得先给渲染器加 shadow map。
}

// ============================================================================
// 七、相机与布光
// ============================================================================
// ## near / far 的取值直接决定远景的深度精度
//
// 一块半径 900 的地面是个“巨大三角形”，深度在三角形内部插值——near 越小、
// far 越大，远处同一像素上的深度分辨率就越粗（near=0.5/far=1200 时约 3mm），
// 台基与地面的微小间隙就会抖成一片碎片。
// 塔高 67、相机距离约 150，near=2 / far=900 已经够用，且精度提高一个量级。
//
// ## 取景的两难（实测数据）
//
// 塔是竖直细长体（67.3 高 / 台基约 34 宽），在 16:9 画面里**竖向才是约束**。
// 两端都试过：
//   · 距离 112（旧值，零余量）→ 塔能撑满，但**塔基被底边裁掉**（读起来像浮在空中）
//   · 距离 159（+30% 余量）→ 完整入画，但主体只占画面 8.4%（显得小）
// 取中间值：距 142 / fov 36 —— 既有余量让台基与塔刹都在框内，又不至于太小。
const camera = new THREE.PerspectiveCamera(36, 16 / 9, 2.0, 900);
camera.position.set(76, 50, 118);
camera.lookAt(0, 33.0, 0);
scene.add(camera);

// ## 布光分层（为何是这个配置）
//
// 木构的“精美”全靠**向光面 / 背光面 / 檐下阴影**三层拉开。
// 环境光给太多会把檐下填平（斗拱就糊成一片），给太少则背光面死黑。
// 所以：压环境、抬主光、补光只负责“不让背面死黑”、轮廓光只负责勾边。
scene.add(new THREE.HemisphereLight(0xa8c8ee, 0x3a2e20, 0.5));
scene.add(new THREE.AmbientLight(0x8fa6c4, 0.22));

// 主光：左前上方，暖白。强度 3.6 是因为塔身是多面体，
// 只有一两个面真正迎着光，其余面靠补光——主光不够就整座塔“平”。
const key = new THREE.DirectionalLight(0xfff0d4, 3.6);
key.position.set(-58, 82, 46);
key.target.position.set(0, 30, 0);
scene.add(key, key.target);

// 补光：右侧偏冷，把背光面从死黑里提起来（不参与造型，只保证看得见）
const fill = new THREE.DirectionalLight(0xa8c0de, 0.85);
fill.position.set(62, 26, 38);
scene.add(fill);

// 轮廓光：后上方，勾出檐口与塔刹的边缘（逆光轮廓是立体感的主要来源）
const rim = new THREE.DirectionalLight(0xb8d0f0, 1.35);
rim.position.set(12, 46, -68);
scene.add(rim);

// 底图反光：从台基下方往上打一点暖光，模拟石台面的漫反射，
// 避免一层檐下与台基之间出现一条死黑带。
const bounce = new THREE.DirectionalLight(0xd8b890, 0.35);
bounce.position.set(-10, -30, 20);
bounce.target.position.set(0, 24, 0);
scene.add(bounce, bounce.target);

scene;
