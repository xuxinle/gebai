// P2：与 p1-flat-nomap.js 完全相同的场景，但声明 compat:'three' —— 走 three.js 同构模型。
//
// 期望（真 three.js r160 实测 76）：
//   线性 = 0.5/π × 1.0 = 0.15915  →  NoToneMapping  →  sRGB  →  8bit = 76
const scene = new THREE.Scene();
scene.background = new THREE.Color(0, 0, 0);
globalThis.renderer = { toneMapping: 0, toneMappingExposure: 1.0, compat: 'three' };

const camera = new THREE.PerspectiveCamera(60, 1, 0.1, 100);
camera.position.set(0, 0, 5);
camera.lookAt(0, 0, 0);
scene.add(camera);

const light = new THREE.DirectionalLight(0xffffff, 1.0);
light.position.set(0, 0, 5);
light.target.position.set(0, 0, 0);
scene.add(light);
scene.add(light.target);

const mesh = new THREE.Mesh(
  new THREE.PlaneGeometry(4, 4),
  new THREE.MeshStandardMaterial({ color: 0x808080, metalness: 0.0, roughness: 1.0 })
);
mesh.name = 'probe_plane';
scene.add(mesh);
scene;
