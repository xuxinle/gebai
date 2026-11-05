
const scene = new THREE.Scene();
scene.background = new THREE.Color(0.05, 0.05, 0.05);
const camera = new THREE.PerspectiveCamera(36.24, 1.375, 0.05, 200);
camera.position.set(1.5,-1.0,26.0);
camera.rotation.set(8*Math.PI/180, 0, 0);
scene.add(camera);
const key = new THREE.DirectionalLight(0xffffff, 0.8125);
key.position.set(-8.6, 5.6, 17.2);
key.target.position.set(0,0,0);
scene.add(key); scene.add(key.target);
const mesh = new THREE.Mesh(new THREE.BoxGeometry(4,4,1),
  new THREE.MeshStandardMaterial({color: new THREE.Color(0.72,0.55,0.24), metalness:0.95, roughness:0.22}));
scene.add(mesh);
scene;
