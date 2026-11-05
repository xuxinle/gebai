const scene = new THREE.Scene();
const g = new THREE.BoxGeometry(1,1,1);
const m = new THREE.Mesh(g, new THREE.MeshStandardMaterial({color:0x99aabb}));
m.position.set(1,2,0);
scene.add(m);
scene;
