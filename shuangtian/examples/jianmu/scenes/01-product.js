// 产品展示：金属球 + 塑料盒 + 挤出底座。
// 这是最典型的一档——"一个产品摆在台面上，三点布光"。
const scene = new THREE.Scene();

const camera = new THREE.PerspectiveCamera(42, 16 / 9, 0.05, 100);
camera.position.set(0, 1.15, 4.4);
camera.lookAt(0, 0.35, 0);
scene.add(camera);

scene.add(new THREE.AmbientLight(0x8899bb, 0.55));

const key = new THREE.DirectionalLight(0xffffff, 2.4);
key.position.set(3.0, 4.5, 3.5);
key.target.position.set(0, 0.3, 0);
scene.add(key, key.target);

const fill = new THREE.DirectionalLight(0x88bbff, 0.75);
fill.position.set(-4.0, 1.5, 2.0);
scene.add(fill);

// 金属球（高金属度 + 低粗糙度 = 锐利高光 + 环境反射）
const ball = new THREE.Mesh(
  new THREE.SphereGeometry(0.62, 48, 32),
  new THREE.MeshStandardMaterial({ color: 0xd09050, metalness: 0.9, roughness: 0.18 })
);
ball.name = 'metal_ball';
ball.position.set(-0.78, 0.68, 0);
scene.add(ball);

// 塑料盒，放进组里验证父子变换
const rig = new THREE.Group();
rig.name = 'box_rig';
rig.position.set(0.8, 0, 0.1);
rig.rotation.y = 0.42;

const box = new THREE.Mesh(
  new THREE.BoxGeometry(0.92, 0.92, 0.92),
  new THREE.MeshStandardMaterial({ color: 0x4a7fd0, metalness: 0.05, roughness: 0.55 })
);
box.name = 'plastic_box';
box.position.set(0, 0.46, 0);
box.rotation.y = 0.3;
rig.add(box);
scene.add(rig);

// 底座：ExtrudeGeometry 从轮廓挤出（模型最常用的建模方式）
const shape = new THREE.Shape();
shape.moveTo(-1.45, -0.68);
shape.lineTo(1.45, -0.68);
shape.lineTo(1.45, 0.68);
shape.lineTo(-1.45, 0.68);
shape.closePath();

const baseGeo = new THREE.ExtrudeGeometry(shape, { depth: 0.1, bevelEnabled: false });
baseGeo.computeVertexNormals();
const base = new THREE.Mesh(
  baseGeo,
  new THREE.MeshStandardMaterial({ color: 0x2c3440, metalness: 0.25, roughness: 0.65 })
);
base.name = 'extruded_base';
base.rotation.x = -Math.PI / 2;   // 挤出方向 +Z → 转成水平
scene.add(base);

scene;
