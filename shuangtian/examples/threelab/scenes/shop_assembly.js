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
// 背景也走灰阶（比墙略深，才能看清外墙轮廓）
scene.background = new THREE.Color(0.052, 0.055, 0.062);

const camera = new THREE.PerspectiveCamera(30, 1100 / 800, 0.05, 400);
// ⚠ Z-up：不给这行整个场景会「躺倒」（见 docs/bpy-assets.md 的踩坑记录）
camera.up.set(0.0, 0.0, 1.0);
camera.position.set(1.55, -10.4, 2.45);
camera.lookAt(-0.15, 0.15, 1.42);
scene.add(camera);

// ---- 单色材质 ----
//
// `MeshBasicMaterial` 在 three 里**不受光照**，所以没有灯也不会有明暗
// 变化——整个块是均匀的一色。
//
// 这对体块模型是**想要的**：轮廓靠几何边界与遮挡区分，不靠阴影。
const TONE_LIGHT = new THREE.MeshBasicMaterial({ color: new THREE.Color(0.52, 0.53, 0.55) });
const TONE_MID = new THREE.MeshBasicMaterial({ color: new THREE.Color(0.30, 0.31, 0.33) });
const TONE_DARK = new THREE.MeshBasicMaterial({ color: new THREE.Color(0.145, 0.150, 0.162) });
const TONE_DEEP = new THREE.MeshBasicMaterial({ color: new THREE.Color(0.065, 0.068, 0.075) });

function spawn(assetName, material) {
  const asset = ASSETS[assetName];
  if (!asset) throw new Error('资产不存在：' + assetName);
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute('position',
    new THREE.BufferAttribute(new Float32Array(asset.positions), 3));
  geometry.setAttribute('normal',
    new THREE.BufferAttribute(new Float32Array(asset.normals), 3));
  if (asset.indices && asset.indices.length > 0) geometry.setIndex(asset.indices);
  const mesh = new THREE.Mesh(geometry, material || TONE_LIGHT);
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
place('floor_slab', 0.0, 0.0, -0.06, { material: TONE_MID });  // 中灰：与墙分开

// 后墙（居中建盒 → z 抬半高）
place('wall_back', 0.0, INNER_D * 0.5 + WALL_T * 0.5, WALL_H * 0.5,
      { material: TONE_MID });

// 左墙（带窗）
place('wall_window', -INNER_W * 0.5 - WALL_T * 0.5, 0.0, WALL_H * 0.5,
      { material: TONE_MID });
// 右墙（实心）
place('wall_side', INNER_W * 0.5 + WALL_T * 0.5, 0.0, WALL_H * 0.5,
      { material: TONE_MID });

// 窗框（深灰，形成边框）
place('window_frame', -INNER_W * 0.5 - WALL_T, 0.0, 1.55, { material: TONE_DARK });

// 屋顶（浅灰，压在墙顶）
place('roof_slab', 0.0, 0.0, WALL_H + 0.05, { material: TONE_MID });

// 招牌（深灰底板）
place('sign_board', 0.0, INNER_D * 0.5 + 0.02, WALL_H + 0.44, { material: TONE_DARK });
// 招牌上的字块（浅灰，做出对比）
place('sign_glyph', 0.0, INNER_D * 0.5 - 0.06, WALL_H + 0.44, { material: TONE_LIGHT });

// 遮阳棚（中灰，18° 斜面）
place('awning', 0.0, INNER_D * 0.5 + 0.52, WALL_H - 0.06,
      { rotation: [0.32, 0.0, 0.0], material: TONE_MID });

// 门槛台阶（浅灰）
place('step', 0.0, -INNER_D * 0.5 - 0.16, 0.08, { material: TONE_LIGHT });

// 门框（深灰，立在前面开口左侧）
place('door_frame', -INNER_W * 0.5 + 0.30, -INNER_D * 0.5 + 0.05, 1.05,
      { material: TONE_DARK });


// ============================================================================
// 店内：货架 + 商品（全部单色）
// ============================================================================

const SHELF_X = -0.72;
const SHELF_Y = INNER_D * 0.5 - 0.24;
place('shelf', SHELF_X, SHELF_Y, 0.0, { material: TONE_DARK });

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
          { material: L % 2 === 0 ? TONE_MID : TONE_DARK });
  }
}

// 货架下方的周转箱
place('crate', SHELF_X - 0.52, SHELF_Y - 0.02, 0.10, { material: TONE_DARK });
place('crate', SHELF_X + 0.42, SHELF_Y - 0.02, 0.10, { material: TONE_DARK });


// ============================================================================
// 店内：柜台 + 收银机
// ============================================================================

const COUNTER_X = 0.92;
const COUNTER_Y = -0.10;
place('counter', COUNTER_X, COUNTER_Y, 0.0, { material: TONE_DARK });
place('counter_top', COUNTER_X, COUNTER_Y, 0.925, { material: TONE_LIGHT });
place('register', COUNTER_X + 0.42, COUNTER_Y - 0.02, 0.955, { material: TONE_DARK });

// 柜台上的两瓶饮料（深灰）
place('bottle_green', COUNTER_X - 0.36, COUNTER_Y + 0.06, 1.075, { material: TONE_DARK });
place('can_red', COUNTER_X - 0.18, COUNTER_Y + 0.10, 1.008, { material: TONE_DEEP });

// 柜台前备货箱
place('crate', COUNTER_X - 0.10, COUNTER_Y - 0.62, 0.10, { material: TONE_DARK });


// ============================================================================
// 店内：冰柜
// ============================================================================

const FRIDGE_X = -INNER_W * 0.5 + 0.62;
const FRIDGE_Y = -0.30;
place('fridge', FRIDGE_X, FRIDGE_Y, 0.0, { material: TONE_LIGHT });
// 玻璃门（最深一档，做出"玻璃"的色块感）
place('fridge_glass', FRIDGE_X, FRIDGE_Y - 0.315, 0.72, { material: TONE_DEEP });

// 冰柜里的饮料（单色）
for (let i = 0; i < 5; ++i) {
  place('bottle_blue', FRIDGE_X - 0.30 + i * 0.15, FRIDGE_Y, 0.62, { material: TONE_DARK });
}
for (let i = 0; i < 4; ++i) {
  place('bottle_green', FRIDGE_X - 0.22 + i * 0.15, FRIDGE_Y, 0.34, { material: TONE_DARK });
}


// ============================================================================
// 室外：路灯 + 门口的箱子堆
// ============================================================================

const LAMP_X = INNER_W * 0.5 + 1.30;
const LAMP_Y = -INNER_D * 0.5 - 0.90;
place('lamp_post', LAMP_X, LAMP_Y, 1.60, { material: TONE_DARK });
place('lamp_head', LAMP_X, LAMP_Y, 3.20, { material: TONE_DARK });

place('crate', -INNER_W * 0.5 - 0.62, -INNER_D * 0.5 - 0.34, 0.10, { material: TONE_MID });
place('crate', -INNER_W * 0.5 - 0.62, -INNER_D * 0.5 - 0.34, 0.31, { material: TONE_DARK });
place('crate', -INNER_W * 0.5 - 0.16, -INNER_D * 0.5 - 0.50, 0.10, { material: TONE_DARK });

scene;
