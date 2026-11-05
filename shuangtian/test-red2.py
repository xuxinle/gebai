import io
import subprocess

code = '''
const ASSETS = __ASSETS__;
const scene = new THREE.Scene();
scene.background = new THREE.Color(0.1, 0.1, 0.1);
const camera = new THREE.PerspectiveCamera(30, 1100/800, 0.05, 400);
camera.up.set(0, 0, 1);
camera.position.set(1.05, -11.6, 2.60);
camera.lookAt(0.15, 0.10, 1.45);
scene.add(camera);
const key = new THREE.DirectionalLight(0xffffff, 1.5);
key.position.set(7, -8.5, 8);
scene.add(key);
const m = new THREE.MeshStandardMaterial({color: new THREE.Color(0xff0000), metalness: 0, roughness: 0.9});
const a = ASSETS['counter'];
const g = new THREE.BufferGeometry();
g.setAttribute('position', new THREE.BufferAttribute(new Float32Array(a.positions), 3));
g.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(a.normals), 3));
if (a.indices) g.setIndex(a.indices);
const mesh = new THREE.Mesh(g, m);
scene.add(mesh);
scene;
'''
io.open('_redtest.js', 'w', encoding='utf-8', newline='\n').write(code)

r = subprocess.run(['py', 'tools/inline_assets.py', '_redtest.js', 'assets_shop.json',
                    '_redtest_full.js'],
                   cwd=r'.', capture_output=True, text=True, encoding='utf-8', errors='replace')
print('inline:', r.stdout.strip()[:150], r.stderr.strip()[:150])

r2 = subprocess.run([r'.\build\dev\bin\align_bench_probe.exe', '_redtest_full.js', '_redtest.png'],
                    cwd=r'.', capture_output=True, text=True, encoding='utf-8', errors='replace')
out = ((r2.stdout or '') + (r2.stderr or '')).strip()
print('render:', ' '.join(l for l in out.split('\n') if '均值' in l or '失败' in l))
