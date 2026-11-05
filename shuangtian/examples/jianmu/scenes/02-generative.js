// 参数化几何：格栅地面 + 旋转体花瓶 + 环绕方块。
// 这一档演示"循环生成"——正是声明式 JSON 表达不了、而 three.js 一句话搞定的事。
const scene = new THREE.Scene();

const camera = new THREE.PerspectiveCamera(38, 16 / 9, 0.05, 100);
camera.position.set(3.2, 2.4, 4.8);
camera.lookAt(0, 0.6, 0);
scene.add(camera);

scene.add(new THREE.AmbientLight(0x6677aa, 0.5));

const key = new THREE.DirectionalLight(0xfff0dd, 2.2);
key.position.set(4, 6, 4);
key.target.position.set(0, 0.5, 0);
scene.add(key, key.target);

const rim = new THREE.PointLight(0xff8844, 2.0, 9);
rim.position.set(-1.2, 1.1, -3);
scene.add(rim);

// —— 格栅地面：5×5 个扁方块 ——
const tileMat = new THREE.MeshStandardMaterial({ color: 0x2a3038, metalness: 0.3, roughness: 0.7 });
for (let ix = -2; ix <= 2; ix++) {
  for (let iz = -2; iz <= 2; iz++) {
    const tile = new THREE.Mesh(new THREE.BoxGeometry(0.88, 0.06, 0.88), tileMat);
    tile.name = 'tile_' + (ix + 2) + '_' + (iz + 2);
    tile.position.set(ix * 0.95, 0, iz * 0.95);
    scene.add(tile);
  }
}

// —— 花瓶：LatheGeometry 旋转体 ——
const profile = [];
for (let i = 0; i <= 20; i++) {
  const t = i / 20;
  const radius = 0.2 + 0.26 * Math.sin(t * Math.PI * 0.9) + 0.05 * Math.cos(t * 6.0);
  profile.push(new THREE.Vector2(Math.max(0.05, radius), t * 1.15));
}
const vase = new THREE.Mesh(
  new THREE.LatheGeometry(profile, 40),
  new THREE.MeshStandardMaterial({ color: 0xc87040, metalness: 0.2, roughness: 0.3 })
);
vase.name = 'lathe_vase';
vase.position.set(0, 0.08, 0);
scene.add(vase);

// —— 环绕方块：父组旋转 + 各自旋转 ——
const orbit = new THREE.Group();
orbit.name = 'orbit_group';
orbit.position.set(0, 0.85, 0);
orbit.rotation.y = 0.4;
scene.add(orbit);

const cubeMat = new THREE.MeshStandardMaterial({ color: 0x4a90d0, metalness: 0.75, roughness: 0.22 });
for (let i = 0; i < 8; i++) {
  const angle = (i / 8) * Math.PI * 2;
  const cube = new THREE.Mesh(new THREE.BoxGeometry(0.17, 0.17, 0.17), cubeMat);
  cube.name = 'orbit_cube_' + i;
  cube.position.set(Math.cos(angle) * 0.98, 0, Math.sin(angle) * 0.98);
  cube.rotation.set(angle, angle * 0.5, 0);
  orbit.add(cube);
}

scene;
