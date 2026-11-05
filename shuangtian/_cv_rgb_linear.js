
const scene = new THREE.Scene();
scene.background = new THREE.Color(0.05, 0.05, 0.05);
const camera = new THREE.PerspectiveCamera(36.24, 1.375, 0.05, 200);
camera.position.set(0,-8,0);
camera.lookAt(0,0,0);
scene.add(camera);
const key = new THREE.DirectionalLight(0xffffff, 2.6);
key.position.set(3,4,6);
key.target.position.set(0,0,0);
scene.add(key); scene.add(key.target);
const mesh = new THREE.Mesh(new THREE.BoxGeometry(1.8,1.8,1.8),
  new THREE.MeshStandardMaterial({color: new THREE.Color(0.72,0.55,0.24), metalness:0.95, roughness:0.22}));
scene.add(mesh);
scene;
