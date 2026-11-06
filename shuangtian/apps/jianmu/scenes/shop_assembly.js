// ============================================================================
// 小卖部 · 纯色版（three.js 组装）
// ----------------------------------------------------------------------------
// 几何来自 bpy 资产（`assets_shop.json`），渲染用 **MeshBasicMaterial**：
// 不受光照、只用单一固有色。观感是扁平的单色块 + 明暗靠面朝向区分——
// 也就是建筑体块模型（massing model）的样子。
//
// ## 为什么用 unlit
//
// 上一版用了 `MeshStandardMaterial` 加三盏灯，颜色是资产自带的（红招牌、
// 蓝周转箱、橙黄商品……）。那是「彩色效果图」，不是想要的。
//
// 纯色体块的价值在于**看清体量与遮挡关系**：没有光照干扰，哪块在前、
// 哪块在后、哪些被挡住，一目了然。这也是方案阶段常用的表达。
//
// ## 配色
//
// 只用两档灰：
//   · 浅灰（0.62）——受光面 / 主要体块：墙、屋顶、地台、货架、柜台
//   · 深灰（0.30）——背光面 / 次要体块：内墙、商品、周转箱
//
// 刻意**不用纯白与纯黑**：太亮会让相邻面糊在一起，太暗会丢失轮廓。
// ============================================================================

const ASSETS = __ASSETS__;

const TAU = 6.283185307179586;

const INNER_W = 3.60;
const INNER_D = 2.40;
const WALL_H = 2.50;
const WALL_T = 0.14;

// ---- 场景 ----
const scene = new THREE.Scene();
// 黄昏天空：偏冷的暗蓝，衬托暖色的铺子
scene.background = new THREE.Color(0.030, 0.038, 0.055);

// ## 色调映射：本场景用 None
//
// 本框架 `Lighting::tone_map` 默认是 **ACES**。ACES 是电影显示变换，
// 代价是**降低饱和度**——低亮度颜色会被洗成灰。实测这套配色（饱和
// 0.8~0.96）在 ACES 下整图发灰。
//
// 用 `scene.toneMapping` 表达（口径与 three 的 `renderer.toneMapping` 一致）：
//   0 = NoToneMapping（直出）
//   4 = ACESFilmic（电影感高光滚降，但颜色会淡）
scene.toneMapping = 0;

const camera = new THREE.PerspectiveCamera(30, 1100 / 800, 0.05, 400);
// ⚠ Z-up：不给这行整个场景会「躺倒」（见 docs/bpy-assets.md 的踩坑记录）
camera.up.set(0.0, 0.0, 1.0);
camera.position.set(1.05, -11.6, 2.60);
camera.lookAt(0.15, 0.10, 1.45);
scene.add(camera);

// ---- 布光 ----
//
// 主光 1.5（第一版 2.1 把暖白墙打成纯白）、补光 0.45 冷蓝给暗部
// 一点颜色（避免死黑）、店内一盏暖黄点光 8.0 照亮货架。
const key = new THREE.DirectionalLight(0xffe9cc, 1.50);
key.position.set(7.0, -8.5, 8.0);
key.target.position.set(0.0, 0.0, 1.2);
scene.add(key);
scene.add(key.target);

const fill = new THREE.DirectionalLight(0x8fa8cc, 0.45);
fill.position.set(-8.0, -6.0, 3.0);
fill.target.position.set(0.0, 0.0, 1.2);
scene.add(fill);
scene.add(fill.target);

// 店内暖光
const interior = new THREE.PointLight(0xffd8a0, 8.0, 5.5);
interior.position.set(-0.2, 0.1, 2.05);
scene.add(interior);

// 冰柜冷光
const fridgeLight = new THREE.PointLight(0xa8d0ff, 3.0, 1.4);
fridgeLight.position.set(-1.40, 0.55, 0.95);
scene.add(fridgeLight);

// ---- 材质（配色）----
//
// 颜色用**十六进制**：hex 会被当作 sRGB 解码成本框架要的线性值。
function mat(hex, metalness, roughness) {
  return new THREE.MeshStandardMaterial({
    color: new THREE.Color(hex),
    metalness: metalness,
    roughness: roughness,
  });
}

const M_WALL = mat(0xded4c2, 0.00, 0.88);   // 暖白墙
const M_WALL_IN = mat(0xc8bda9, 0.00, 0.90);   // 内墙（深半档 → 进深）
const M_ROOF = mat(0x6e737d, 0.55, 0.45);      // 铁皮屋顶
const M_FLOOR = mat(0x8a8378, 0.00, 0.80);     // 水泥地
const M_WOOD = mat(0x6b4326, 0.00, 0.62);      // 木作
const M_WOOD_D = mat(0x4a2d19, 0.00, 0.65);      // 深木
const M_RED = mat(0x9c2a20, 0.00, 0.58);      // 招牌红
const M_CREAM = mat(0xefe6d2, 0.00, 0.55);      // 招牌米
const M_METAL = mat(0x4a4f57, 0.85, 0.35);      // 金属深灰
const M_GLASS = mat(0x7d9aa8, 0.20, 0.08);      // 玻璃
const M_CRATE = mat(0x2f5a6b, 0.00, 0.70);      // 周转箱

// 商品：低饱和色 + 替换档（成排时形成色彩纹理）
const M_BOTTLE_G = mat(0x3a6b42, 0.00, 0.22);
const M_BOTTLE_B = mat(0x33526e, 0.00, 0.22);
const M_CAN_R = mat(0xa8442f, 0.70, 0.30);
const M_CAN_Y = mat(0xc09040, 0.70, 0.30);
const M_BOX = mat(0x9a6b32, 0.00, 0.72);
const M_BAG = mat(0xb04e38, 0.00, 0.55);

function spawn(assetName, material) {
  const asset = ASSETS[assetName];
  if (!asset) throw new Error('资产不存在：' + assetName);
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute('position',
    new THREE.BufferAttribute(new Float32Array(asset.positions), 3));
  geometry.setAttribute('normal',
    new THREE.BufferAttribute(new Float32Array(asset.normals), 3));
  if (asset.indices && asset.indices.length > 0) geometry.setIndex(asset.indices);
  const mesh = new THREE.Mesh(geometry, material || M_WALL);
  mesh.name = assetName;
  return mesh;
}

function place(assetName, x, y, z, opts) {
  const mesh = spawn(assetName, opts && opts.material);
  mesh.position.set(x, y, z);
  if (opts && opts.rotation) mesh.rotation.set(opts.rotation[0], opts.rotation[1],
                                               opts.rotation[2]);
  if (opts && opts.scale) mesh.scale.set(opts.scale[0], opts.scale[1], opts.scale[2]);
  scene.add(mesh);
  return mesh;
}


// ============================================================================
// 建筑主体
// ============================================================================

// 地台（浅灰，最亮的一档——它是"接地"的面）
place('floor_slab', 0.0, 0.0, -0.06, { material: M_FLOOR });

// ⚠ **每个资产的 z 原点不同**（这一版踩到）
//
// bpy 资产脚本里三种建法给出三种 z 范围：
//
// | 建法 | z 范围 | 例子 |
// |---|---|---|
// | `_add_box`（居中） | [-H/2, +H/2] | wall_back / wall_side / roof_slab |
// | `make_window_wall`（`slab` 按区间） | [0, H] | **wall_window** |
// | 手工 `_add_ring` 从 0 起 | [0, H] | shelf / counter / fridge / lamp_post |
//
// 所以定位时不能一律写 `WALL_H * 0.5`：
//
//   · 居中建盒 → 中心在 H/2，直接放 `WALL_H * 0.5`
//   · 从地面长起 → 底面在 0，也要放 `WALL_H * 0.5`（巧合一致）
//
// 两者数值恰好相同，但**含义不同**——一旦某个资产的建法变了（比如
// `wall_window` 从居中改成按区间），就会错半个高度。
//
// 本次就是：`wall_window` 重写后变成 [0, 2.50]，而代码里按居中算，
// 于是那面墙**沉到地下 1.25**——画面上左侧墙只剩一个尖角伸出来。

// 后墙（居中建盒，z ∈ [-1.25, 1.25]）
place('wall_back', 0.0, INNER_D * 0.5 + WALL_T * 0.5, WALL_H * 0.5,
      { material: M_WALL_IN });

// 左墙（带窗）——**从地面长起**，z ∈ [0, 2.50]
// 底面已在 0，所以 z 直接放 0（不是 WALL_H * 0.5）
place('wall_window', -INNER_W * 0.5 - WALL_T * 0.5, 0.0, 0.0,
      { material: M_WALL });
// 右墙（居中建盒，z ∈ [-1.25, 1.25]）
place('wall_side', INNER_W * 0.5 + WALL_T * 0.5, 0.0, WALL_H * 0.5,
      { material: M_WALL });

// 窗框（深灰，形成边框）
// 窗框：贴在左墙**内侧**（相机从右前方看进来，外侧表面看不到）
// 上一版放在外墙（x = -W/2 - WALL_T），朝向背离相机，等于没画。
place('window_frame', -INNER_W * 0.5 + 0.02, 0.0, 1.55, { material: M_METAL });

// 后墙墙裙：贴在后墙内侧的一块深色板，制造"墙有厚度/店有进深"的层次。
// 屋顶（压在墙顶）
place('roof_slab', 0.0, 0.0, WALL_H + 0.05, { material: M_ROOF });

// 招牌：立在屋顶**上方**、朝**前**（y 负方向）。
//
// ⚠ 第一版放在 `INNER_D * 0.5 + 0.02`（后墙外侧）——那是**背面**，
// 相机从前方看不到，而且被遮阳棚完全遮住。招牌必须朝前。
place('sign_board', 0.0, -INNER_D * 0.5 - 0.16, WALL_H + 0.62, { material: M_RED });
place('sign_glyph', 0.0, -INNER_D * 0.5 - 0.26, WALL_H + 0.62, { material: M_CREAM });

// 遮阳棚：从**前檐**向外伸出（不是后墙外）。
//
// 高度压到 WALL_H - 0.55（而不是贴屋顶）：贴着屋顶会与招牌争位置，
// 而且从正面看会盖住整个门面。
place('awning', 0.0, -INNER_D * 0.5 - 0.58, WALL_H - 0.55,
      { rotation: [0.55, 0.0, 0.0], material: M_RED, scale: [1.0, 1.15, 1.0] });
// 棚前沿的米色边条（第二层薄板）
place('awning', 0.0, -INNER_D * 0.5 - 1.12, WALL_H - 0.86,
      { rotation: [0.55, 0.0, 0.0], material: M_CREAM, scale: [1.0, 0.12, 1.0] });

// 门槛台阶（浅灰）
place('step', 0.0, -INNER_D * 0.5 - 0.16, 0.08, { material: M_FLOOR });

// 门框（深灰，立在前面开口左侧）
// 门框去掉（前开口本身已暗示入口，立柱子反而挡视线）


// ============================================================================
// 店内：货架 + 商品（全部单色）
// ============================================================================

const SHELF_X = -0.42;
const SHELF_Y = INNER_D * 0.5 - 0.24;
place('shelf', SHELF_X, SHELF_Y, 0.0, { material: M_WOOD_D });

// 商品：**统一深灰**，不按品类上色。
//
// 上一版给瓶子绿、罐子红黄、盒子棕、袋子红——那读起来像超市宣传图。
// 单色后商品只表达"层板上摆满了东西"这层信息，不抢体块关系。
const SHELF_W = 1.60;
const SHELF_D = 0.42;
const LEVELS = [
  { z: 0.10, asset: 'bottle_green', count: 9, gap: 0.155, lift: 0.12 },
  { z: 0.46, asset: 'can_red', count: 11, gap: 0.130, lift: 0.058 },
  { z: 0.82, asset: 'box_item', count: 8, gap: 0.180, lift: 0.11 },
  { z: 1.18, asset: 'bag_item', count: 7, gap: 0.205, lift: 0.12 },
];

for (let L = 0; L < LEVELS.length; ++L) {
  const level = LEVELS[L];
  for (let i = 0; i < level.count; ++i) {
    const x = SHELF_X - SHELF_W * 0.5 + 0.16 + i * level.gap;
    if (x > SHELF_X + SHELF_W * 0.5 - 0.10) break;
    const y = SHELF_Y - SHELF_D * 0.5 + 0.13;
    place(level.asset, x, y, level.z + 0.030 + level.lift,
          { material: L === 0 ? M_BOTTLE_G : (L === 1 ? M_CAN_R : (L === 2 ? M_BOX : M_BAG)) });
  }
}

// 货架下方的周转箱
place('crate', SHELF_X - 0.52, SHELF_Y - 0.02, 0.10, { material: M_CRATE });
place('crate', SHELF_X + 0.42, SHELF_Y - 0.02, 0.10, { material: M_CRATE });


// ============================================================================
// 店内：柜台 + 收银机
// ============================================================================

const COUNTER_X = 1.16;
const COUNTER_Y = -0.10;
place('counter', COUNTER_X, COUNTER_Y, 0.0, { material: M_WOOD });
place('counter_top', COUNTER_X, COUNTER_Y, 0.925, { material: M_CREAM });
place('register', COUNTER_X + 0.42, COUNTER_Y - 0.02, 0.955, { material: M_METAL });

// 柜台上的两瓶饮料（深灰）
place('bottle_green', COUNTER_X - 0.36, COUNTER_Y + 0.06, 1.075, { material: M_BOTTLE_G });
place('can_red', COUNTER_X - 0.18, COUNTER_Y + 0.10, 1.008, { material: M_CAN_R });

// 柜台前备货箱
place('crate', COUNTER_X - 0.10, COUNTER_Y - 0.62, 0.10, { material: M_CRATE });


// ============================================================================
// 店内：冰柜
// ============================================================================

const FRIDGE_X = -INNER_W * 0.5 + 0.40;
const FRIDGE_Y = 0.55;
place('fridge', FRIDGE_X, FRIDGE_Y, 0.0,
      { material: M_WALL, scale: [0.52, 0.52, 0.52] });
// 玻璃门（最深一档，做出"玻璃"的色块感）
place('fridge_glass', FRIDGE_X, FRIDGE_Y - 0.164, 0.375,
      { material: M_GLASS, scale: [0.52, 0.52, 0.52] });

// 冰柜里的饮料（单色）
for (let i = 0; i < 5; ++i) {
  place('bottle_blue', FRIDGE_X - 0.15 + i * 0.075, FRIDGE_Y, 0.33,
        { material: M_BOTTLE_B, scale: [0.7, 0.7, 0.7] });
}
for (let i = 0; i < 4; ++i) {
  place('bottle_green', FRIDGE_X - 0.11 + i * 0.075, FRIDGE_Y, 0.18,
        { material: M_BOTTLE_G, scale: [0.7, 0.7, 0.7] });
}


// ============================================================================
// 室外：路灯 + 门口的箱子堆
// ============================================================================

const LAMP_X = INNER_W * 0.5 + 1.30;
const LAMP_Y = -INNER_D * 0.5 - 0.90;
place('lamp_post', LAMP_X, LAMP_Y, 1.60, { material: M_METAL });
place('lamp_head', LAMP_X, LAMP_Y, 3.20, { material: M_METAL });

place('crate', -INNER_W * 0.5 - 0.62, -INNER_D * 0.5 - 0.34, 0.10, { material: M_CRATE });
place('crate', -INNER_W * 0.5 - 0.62, -INNER_D * 0.5 - 0.34, 0.31, { material: M_CRATE });
place('crate', -INNER_W * 0.5 - 0.16, -INNER_D * 0.5 - 0.50, 0.10, { material: M_CRATE });

scene;
