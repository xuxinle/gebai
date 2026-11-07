// ============================================================================
// 阴影对标场景 · three.js 版
// ----------------------------------------------------------------------------
// ## 为什么需要这份场景
//
// `quad_parity.js` / `flat_parity.js` 都**没有阴影**。而 2026-11-07 我改了
// 阴影贴图的偏置算法（改成法线偏移，修自遮挡圆环 + 保地面接触阴影），
// 那批改动**没有被任何 three 对标场景覆盖**——单元测试过了，但那是自己
// 跟自己对，不是跟浏览器真值对。
//
// ## 场景设计：让偏置的两个需求同时受检
//
// | 物体 | 检验什么 |
// |---|---|
// | 球（曲率高）| **自遮挡圆环**：偏置太小会出同心环 |
// | 立方体（贴地）| **接触阴影**：偏置太大会让影子脱开物体 |
// | 地面 | 接收两者的投影 |
//
// 球与盒同时在场，才能一次看出偏置是否两头都顾上。
//
// ## 阴影参数（必须尽量与 three 的默认一致）
//
// three 的 `PCFSoftShadowMap` 用 2048² 贴图 + 基于纹素的 PCF。
// 本框架的 `shadow_map.resolution` 对应前者；`soft_radius` 对应 PCF 半径。
// ============================================================================

const scene = new THREE.Scene();
scene.background = new THREE.Color(0.05, 0.05, 0.05);

const camera = new THREE.PerspectiveCamera(34, 1100 / 800, 0.05, 200);
camera.position.set(0.4, -8.5, 4.6);
camera.lookAt(0.0, 0.0, 0.6);
scene.add(camera);

// ---- 主光：平行光，从右上后方照来 → 影子朝左下前方 ----
const key = new THREE.DirectionalLight(0xffffff, 2.4);
key.position.set(3.5, 4.5, 6.0);
key.target.position.set(0, 0, 0);
key.castShadow = true;
key.shadow.mapSize.set(2048, 2048);
key.shadow.camera.left = -6;
key.shadow.camera.right = 6;
key.shadow.camera.top = 6;
key.shadow.camera.bottom = -6;
key.shadow.camera.near = 0.5;
key.shadow.camera.far = 40;
key.shadow.bias = -0.0005;
key.shadow.normalBias = 0.02;
scene.add(key);
scene.add(key.target);

// ---- 地面：接收阴影 ----
const ground = new THREE.Mesh(
  new THREE.PlaneGeometry(20, 20),
  new THREE.MeshStandardMaterial({ color: 0xbcb8b0, roughness: 0.85, metalness: 0.0 })
);
ground.rotation.x = -Math.PI / 2;
ground.receiveShadow = true;
ground.name = 'ground';
scene.add(ground);

// ---- 球：检验自遮挡（曲率高）----
const sphere = new THREE.Mesh(
  new THREE.SphereGeometry(1.0, 48, 24),
  new THREE.MeshStandardMaterial({ color: 0x4a6fa5, roughness: 0.45, metalness: 0.0 })
);
sphere.position.set(-1.6, 1.0, 0.2);
sphere.castShadow = true;
sphere.receiveShadow = true;
sphere.name = 'sphere';
scene.add(sphere);

// ---- 立方体：检验接触阴影（贴地、平底）----
const box = new THREE.Mesh(
  new THREE.BoxGeometry(1.5, 1.5, 1.5),
  new THREE.MeshStandardMaterial({ color: 0xb8522e, roughness: 0.55, metalness: 0.0 })
);
box.position.set(1.7, 0.75, 0.0);
box.rotation.set(0.0, 0.42, 0.0);
box.castShadow = true;
box.receiveShadow = true;
box.name = 'box';
scene.add(box);

// ---- 环境光：给暗部一点底，避免纯黑（两边同一口径）----
const ambient = new THREE.AmbientLight(0xffffff, 0.35);
scene.add(ambient);

// 阴影必须显式开这个类型才有 PCFSoft
globalThis.renderer = {
  toneMapping: 0,
  toneMappingExposure: 1.0,
  compat: 'three',
  shadowMapEnabled: true,
  shadowMapType: 'PCFSoft',
};
scene;
