// three.js 参照渲染：与 `bpy_watch.py` 同构的轮系。
//
// ## 为什么要单独一份
//
// `reference_page.js` 已经有一套齿轮生成逻辑，但那是"凭印象写的同构"，
// 而且相机/光照参数没和 bpy 脚本严格对上（踩过：相机角度不同 → 两张图
// 构图完全不同，所有对比无效）。
//
// 本文件**逐项照抄 `bpy_watch.py` 的数值**，并在注释里标明对应关系。
//
// ## 用法
//
// 由 `reference.html` 引入，调用 `window.renderWatch(opts)`。

import * as THREE from './three_browser.js';

const TAU = 6.283185307179586;
const MODULE = 0.21;

/** 齿轮外轮廓（与 bpy_watch.py 的 gear_profile 同一算法）。 */
function gearProfile(teeth, module, rimWidth, toothFraction = 0.44) {
  const pitchRadius = (module * teeth) / 2.0;
  const tipRadius = pitchRadius + rimWidth * 0.5;
  const rootRadius = pitchRadius - rimWidth * 0.5;
  const pitchAngle = TAU / teeth;
  const toothAngle = pitchAngle * toothFraction;
  const profile = [];
  for (let i = 0; i < teeth; ++i) {
    const base = i * pitchAngle;
    const a0 = base - toothAngle * 0.5;
    const a1 = base - toothAngle * 0.30;
    const a2 = base + toothAngle * 0.30;
    const a3 = base + toothAngle * 0.5;
    profile.push([Math.cos(a0) * rootRadius, Math.sin(a0) * rootRadius]);
    profile.push([Math.cos(a1) * tipRadius, Math.sin(a1) * tipRadius]);
    profile.push([Math.cos(a2) * tipRadius, Math.sin(a2) * tipRadius]);
    profile.push([Math.cos(a3) * rootRadius, Math.sin(a3) * rootRadius]);
    const gapStart = a3;
    const gapEnd = base + pitchAngle - toothAngle * 0.5;
    for (let s = 1; s < 4; ++s) {
      const a = gapStart + (gapEnd - gapStart) * (s / 3.0);
      profile.push([Math.cos(a) * rootRadius, Math.sin(a) * rootRadius]);
    }
  }
  return { profile, pitchRadius, rootRadius };
}

/** 建一个齿轮（挤出轮廓 + 轴孔 + 轮辐窗口）。 */
function makeGear(teeth, module, rimWidth, depth, bore, spokes) {
  const { profile, pitchRadius, rootRadius } = gearProfile(teeth, module, rimWidth);
  const shape = new THREE.Shape();
  profile.forEach(([x, y], i) => (i === 0 ? shape.moveTo(x, y) : shape.lineTo(x, y)));
  shape.closePath();

  // 轴孔
  const holePath = new THREE.Path();
  holePath.absarc(0, 0, bore, 0, TAU, true);
  shape.holes.push(holePath);

  // 轮辐窗口（与 bpy 版同算法：spoke_width = span * 0.34/(spokes/5)）
  const innerRadius = Math.max(rootRadius - rimWidth * 1.15, bore * 2.0);
  const hubRadius = Math.max(bore * 2.8, rootRadius * 0.26);
  if (spokes > 0) {
    const span = TAU / spokes;
    const spokeWidth = span * (0.34 / Math.max(1.0, spokes / 5.0));
    for (let s = 0; s < spokes; ++s) {
      const start = s * span + spokeWidth * 0.5;
      const end = (s + 1) * span - spokeWidth * 0.5;
      const window = new THREE.Path();
      const segs = 6;
      for (let k = segs; k >= 0; --k) {
        const a = start + (end - start) * (k / segs);
        const px = Math.cos(a) * innerRadius;
        const py = Math.sin(a) * innerRadius;
        if (k === segs) window.moveTo(px, py);
        else window.lineTo(px, py);
      }
      for (let k = 0; k <= segs; ++k) {
        const a = start + (end - start) * (k / segs);
        window.lineTo(Math.cos(a) * hubRadius, Math.sin(a) * hubRadius);
      }
      window.closePath();
      shape.holes.push(window);
    }
  }

  const geometry = new THREE.ExtrudeGeometry(shape, {
    depth,
    bevelEnabled: false,
    curveSegments: 12,
  });
  geometry.translate(0, 0, -depth * 0.5);
  geometry.computeVertexNormals();
  return { geometry, pitchRadius };
}

function pbr(color, metallic, roughness) {
  return new THREE.MeshStandardMaterial({
    color: new THREE.Color(color[0], color[1], color[2]),
    metalness: metallic,
    roughness,
  });
}

/** 与 bpy_watch.py 同参数的轮系场景。 */
window.renderWatch = async function renderWatch(options) {
  const width = (options && options.width) || 1100;
  const height = (options && options.height) || 800;
  const toneMapping = (options && options.toneMapping) || 'none';

  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  document.body.appendChild(canvas);

  const renderer = new THREE.WebGLRenderer({
    canvas,
    antialias: true,
    preserveDrawingBuffer: true,
  });
  renderer.setSize(width, height, false);
  renderer.setPixelRatio(1);
  const map = {
    none: THREE.NoToneMapping,
    aces: THREE.ACESFilmicToneMapping,
    agx: THREE.AgXToneMapping,
  };
  renderer.toneMapping = map[toneMapping] !== undefined ? map[toneMapping] : THREE.NoToneMapping;
  renderer.toneMappingExposure = 1.0;
  renderer.outputColorSpace = THREE.SRGBColorSpace;

  const scene = new THREE.Scene();
  // 世界背景：与 bpy 版 0.05 中性灰一致（three 的 Color 接受线性值）
  scene.background = new THREE.Color(0.05, 0.05, 0.05);

  // 相机：与 bpy_watch.py 一致（lens 55 / 位置 (1.5,-1.0,26) / 俯 8°）
  // three 的 fov 是**垂直** fov；bpy 的 55mm 在全画幅（24mm 高）上换算：
  //   fov_y = 2*atan(24/(2*55)) = 24.6°
  const fovY = 2 * Math.atan(24.0 / (2.0 * 55.0)) * 180 / Math.PI;
  const camera = new THREE.PerspectiveCamera(fovY, width / height, 0.05, 200.0);
  camera.position.set(1.5, -1.0, 26.0);
  camera.rotation.set(0, 0, 0);
  camera.rotation.x = 8.0 * Math.PI / 180.0;
  camera.updateMatrixWorld();
  // 相机默认朝 -Z；旋转 8° 后目标稍偏下
  const target = new THREE.Vector3(
    1.5,
    -1.0 + 26.0 * Math.tan(8.0 * Math.PI / 180.0),
    0.0
  );
  camera.lookAt(target);

  // 主光：bpy 的 SUN energy 2600。
  //
  // ## 这个换算必须显式说明
  //
  // Blender 的 SUN `energy` 是 W/m²，three 的 `DirectionalLight.intensity`
  // 是**无量纲相对值**。两者无官方换算，只能实测标定。
  //
  // 本框架把 energy 2600 映射成 `diffuse = 2.00`（见 `bpy_runtime.cpp`
  // 的注释）。three 侧取同一个"视觉亮度"对应的 intensity：实测 2600/3200
  // = 0.8125 让主体均值落在 78 附近（与 Blender 参照一致）。
  const sun = new THREE.DirectionalLight(0xffffff, 0.8125);
  // 光位置：与 bpy_watch.py 的 lamp_obj.location 一致
  sun.position.set(-8.6, 5.6, 17.2);
  sun.target.position.set(0, 0, 0);
  scene.add(sun);
  scene.add(sun.target);

  // 材质（与 bpy_watch.py 的 MAT_* 同值）
  const brass = pbr([0.72, 0.55, 0.24], 0.95, 0.22);
  const steel = pbr([0.28, 0.34, 0.46], 0.92, 0.18);
  const copper = pbr([0.66, 0.32, 0.18], 0.45, 0.35);
  const gold = pbr([0.90, 0.72, 0.30], 0.98, 0.15);

  const SPECS = [
    ['barrel_wheel', 42, MODULE, 0.34, 0.30, 5, brass],
    ['center_wheel', 26, MODULE, 0.24, 0.26, 5, brass],
    ['third_wheel', 18, MODULE, 0.20, 0.22, 4, gold],
    ['fourth_wheel', 14, MODULE, 0.17, 0.20, 4, gold],
    ['escape_wheel', 15, MODULE * 0.86, 0.15, 0.13, 5, steel],
  ];
  const IDLERS = [
    ['idler_a', 12, MODULE * 0.9, 0.16, 0.13, 6, brass],
    ['idler_b', 10, MODULE * 0.9, 0.16, 0.13, 5, copper],
    ['idler_c', 16, MODULE * 0.9, 0.16, 0.13, 5, brass],
  ];

  const built = [];
  for (const [name, teeth, module, rim, depth, spokes, material] of SPECS) {
    const { geometry, pitchRadius } = makeGear(teeth, module, rim, depth, 0.11, spokes);
    const mesh = new THREE.Mesh(geometry, material);
    mesh.name = name;
    scene.add(mesh);
    built.push({ mesh, pitchRadius, teeth, module });
  }
  for (const [name, teeth, module, rim, depth, spokes, material] of IDLERS) {
    const { geometry, pitchRadius } = makeGear(teeth, module, rim, depth, 0.11, spokes);
    const mesh = new THREE.Mesh(geometry, material);
    mesh.name = name;
    scene.add(mesh);
    built.push({ mesh, pitchRadius, teeth, module });
  }

  // 布局：与 bpy_watch.py 完全一致
  built[0].mesh.position.set(-1.2, 0.9, 0);
  built[0].mesh.rotation.z = 0;

  function placePair(prev, curr, angleDeg, baseZ) {
    const distance = prev.module * (prev.teeth + curr.teeth) / 2.0;
    const rad = (angleDeg * Math.PI) / 180.0;
    curr.mesh.position.set(
      prev.mesh.position.x + Math.cos(rad) * distance,
      prev.mesh.position.y + Math.sin(rad) * distance,
      baseZ
    );
    curr.mesh.rotation.z = rad + Math.PI / curr.teeth;
  }
  placePair(built[0], built[1], 205.0, 0.05);
  placePair(built[1], built[2], 150.0, 0.10);
  placePair(built[2], built[3], 95.0, 0.15);
  placePair(built[3], built[4], 40.0, 0.20);

  const idlerPositions = [[5.4, -3.2, 0.30], [-5.8, 3.4, 0.34], [6.6, 2.8, 0.26]];
  for (let i = 0; i < 3; ++i) {
    built[5 + i].mesh.position.set(...idlerPositions[i]);
    built[5 + i].mesh.rotation.z = i * 0.7;
  }

  renderer.render(scene, camera);

  const gl = renderer.getContext();
  const raw = new Uint8Array(width * height * 4);
  gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, raw);

  return {
    width,
    height,
    bytes: Array.from(raw),
    toneMapping,
    triangleCount: (() => {
      let n = 0;
      scene.traverse((o) => {
        if (o.isMesh && o.geometry.index) n += o.geometry.index.count / 3;
        else if (o.isMesh) n += o.geometry.attributes.position.count / 3;
      });
      return Math.round(n);
    })(),
  };
};

window.__ready = true;
