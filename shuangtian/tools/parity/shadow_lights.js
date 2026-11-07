// ============================================================================
// 阴影对标 · 多光型 + 多机位（three.js 版）
// ----------------------------------------------------------------------------
// ## 与 `shadow_parity.js` 的分工
//
// | 文件 | 覆盖 |
// |---|---|
// | `shadow_parity.js` | 一盏平行光，固定机位——查**偏置/自遮挡/接触阴影** |
// | `shadow_lights.js`（本文件）| 点光 / 聚光 / 三盏并存——查**光型语义** |
//
// ## 为何要单独一份
//
// 平行光的阴影最简单：一张正交深度图就够。而：
//
// | 光型 | 阴影实现 | 典型 bug |
// |---|---|---|
// | 平行光 | 正交投影 | 偏置、方向 |
// | 聚光 | 透视投影 + 锥角 | 锥外不该有影、衰减 |
// | 点光 | **需要 cubemap 六面** | 两条腿都主动跳过——得验证“跳过”是一致的 |
//
// 点光是个特例：本框架两条腿都用**单张**深度图，所以点光阴影
// **不支持**（会静默跳过）。这里把它放进场景，是为了钉住
// **「浏览器有点光影子、我们没有」这个已知差异**——而不是假装解决了。
//
// ## 三盏灯的分工
//
// - `key`（平行光，castShadow）→ 主阴影，与 `shadow_parity.js` 同口径
// - `spot`（聚光，castShadow）→ 第二套阴影，检验透视深度图
// - `point`（点光，castShadow）→ **已知不支持**，用来钉住差异
// ============================================================================

const scene = new THREE.Scene();
scene.background = new THREE.Color(0.05, 0.05, 0.05);

const camera = new THREE.PerspectiveCamera(36, 1100 / 800, 0.05, 200);
camera.position.set(0.0, -9.5, 6.2);
camera.lookAt(0.0, 0.0, 0.5);
scene.add(camera);

// ---- 地面 ----
const ground = new THREE.Mesh(
  new THREE.PlaneGeometry(24, 24),
  new THREE.MeshStandardMaterial({ color: 0xbcb8b0, roughness: 0.9, metalness: 0.0 })
);
ground.rotation.x = -Math.PI / 2;
ground.receiveShadow = true;
ground.name = 'ground';
scene.add(ground);

// ---- 平行光：主阴影 ----
const key = new THREE.DirectionalLight(0xffffff, 1.9);
key.position.set(4.0, 5.0, 5.0);
key.target.position.set(0, 0, 0);
key.castShadow = true;
key.shadow.mapSize.set(1024, 1024);
key.shadow.camera.left = -7;
key.shadow.camera.right = 7;
key.shadow.camera.top = 7;
key.shadow.camera.bottom = -7;
key.shadow.camera.near = 0.5;
key.shadow.camera.far = 40;
scene.add(key);
scene.add(key.target);

// ---- 聚光：从高处斜下打，锥内投影 ----
const spot = new THREE.SpotLight(0xffd0a0, 3.0, 30, Math.PI / 7, 0.35, 1.5);
spot.position.set(-4.0, 6.0, 2.0);
spot.target.position.set(-1.5, 0, 0);
spot.castShadow = true;
spot.shadow.mapSize.set(1024, 1024);
spot.shadow.camera.near = 0.5;
spot.shadow.camera.far = 30;
scene.add(spot);
scene.add(spot.target);

// ---- 点光：**已知不支持阴影**（两条腿都用单张深度图）----
const point = new THREE.PointLight(0x88bbff, 1.2, 20, 2);
point.position.set(3.0, 2.2, -1.0);
point.castShadow = true;
point.shadow.mapSize.set(512, 512);
scene.add(point);

// ---- 三个物体：两个球（高曲率，验自遮挡）+ 一个盒（平底，验接触阴影）----
const sphereA = new THREE.Mesh(
  new THREE.SphereGeometry(0.9, 40, 20),
  new THREE.MeshStandardMaterial({ color: 0x4a6fa5, roughness: 0.5, metalness: 0.0 })
);
sphereA.position.set(-1.5, 0.9, 0.0);
sphereA.castShadow = true;
sphereA.receiveShadow = true;
sphereA.name = 'sphereA';
scene.add(sphereA);

const sphereB = new THREE.Mesh(
  new THREE.SphereGeometry(0.7, 40, 20),
  new THREE.MeshStandardMaterial({ color: 0x7a9a4a, roughness: 0.6, metalness: 0.0 })
);
sphereB.position.set(-3.2, 0.7, -1.2);
sphereB.castShadow = true;
sphereB.receiveShadow = true;
sphereB.name = 'sphereB';
scene.add(sphereB);

const box = new THREE.Mesh(
  new THREE.BoxGeometry(1.3, 1.3, 1.3),
  new THREE.MeshStandardMaterial({ color: 0xb8522e, roughness: 0.55, metalness: 0.0 })
);
box.position.set(2.0, 0.65, 0.3);
box.rotation.set(0.0, 0.5, 0.0);
box.castShadow = true;
box.receiveShadow = true;
box.name = 'box';
scene.add(box);

// ---- 环境：半球光（新实现的方向插值也要在这里受检）----
const ambient = new THREE.HemisphereLight(0x9aa8bc, 0x50493f, 0.5);
scene.add(ambient);

globalThis.renderer = {
  toneMapping: 0,
  toneMappingExposure: 1.0,
  compat: 'three',
  shadowMapEnabled: true,
  shadowMapType: 'PCFSoft',
};
scene;
