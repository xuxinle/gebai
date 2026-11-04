// 材质对照表：一排不同金属度/粗糙度的球，用来看 PBR 参数的实际效果。
// 用途很实在：调材质时不用反复改一个球，一眼能对比。
const scene = new THREE.Scene();

const camera = new THREE.PerspectiveCamera(40, 16 / 9, 0.05, 100);
camera.position.set(0, 0.8, 5.2);
camera.lookAt(0, 0.2, 0);
scene.add(camera);

scene.add(new THREE.AmbientLight(0x7788aa, 0.5));

const key = new THREE.DirectionalLight(0xffffff, 2.6);
key.position.set(2.5, 4.0, 3.5);
scene.add(key);

const rim = new THREE.DirectionalLight(0x88aaff, 1.1);
rim.position.set(-3.5, 1.0, -2.5);
scene.add(rim);

// 4 列（金属度）× 3 行（粗糙度）
const metalness_values = [0.0, 0.35, 0.7, 1.0];
const roughness_values = [0.08, 0.35, 0.8];

for (let row = 0; row < roughness_values.length; row++) {
  for (let col = 0; col < metalness_values.length; col++) {
    const metalness = metalness_values[col];
    const roughness = roughness_values[row];
    const ball = new THREE.Mesh(
      new THREE.SphereGeometry(0.34, 40, 28),
      new THREE.MeshStandardMaterial({
        color: 0xd8b070,
        metalness: metalness,
        roughness: roughness
      })
    );
    ball.name = 'm' + metalness.toFixed(2) + '_r' + roughness.toFixed(2);
    ball.position.set((col - 1.5) * 0.95, (1 - row) * 0.85, 0);
    scene.add(ball);
  }
}

// 底板
const plate = new THREE.Mesh(
  new THREE.BoxGeometry(4.2, 0.08, 1.6),
  new THREE.MeshStandardMaterial({ color: 0x262c36, metalness: 0.2, roughness: 0.8 })
);
plate.name = 'plate';
plate.position.set(0, -1.28, 0);
scene.add(plate);

scene;
